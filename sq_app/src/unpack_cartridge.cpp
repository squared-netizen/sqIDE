// sqterm: unpacking a package that is a squared cartridge, 3.0 (sqcart's
// spec, Appendix B.2: kind "package"). pkg calls it for a .sq; the tarball
// reader (unpack.cpp) stays for .tar.zst and pkg backup's .tar.
//
// This file is YOURS (ownership class: seeded, sqterm's own).
//
// Every entry is read through sqcart's reader, verified against its digest
// as it is read (a chunked entry 1 MiB at a time), and written to a part
// file renamed into place: mode 0755 when its flags have x, else 0644.
// Entries under SQ-INF/ are the package's, not the prefix's: the manifest
// goes to `meta`. The symbolic links a package installs are in its
// manifest (consumers.sqterm.links, path to target), made last; one whose
// path or target would leave the prefix is refused, as the tar reader
// refuses an absolute name or a "..".
//
// Modules (sq.cart reads through `import std`); its interface is
// unpack.hpp's, which names no standard type, so pkg's files, built from
// standard headers, call it as they call unpack_archive.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unpack.hpp"

import std;
import sq.data;
import sq.cart;

namespace {

/// Each segment of `path` is a name: no empty one, no "." or "..".
bool plain_path(std::string_view path) {
    if (path.empty() || path.front() == '/') return false;
    for (std::size_t at = 0; at <= path.size();) {
        const std::size_t slash = std::min(path.find('/', at), path.size());
        const std::string_view part = path.substr(at, slash - at);
        if (part.empty() || part == "." || part == "..") return false;
        at = slash + 1;
    }
    return true;
}

/// Whether a link at `path` to `target` stays in the prefix: `target` is
/// relative, and, read from the link's folder, never climbs above the root.
bool target_inside(std::string_view path, std::string_view target) {
    if (target.empty() || target.front() == '/') return false;
    long depth = static_cast<long>(std::ranges::count(path, '/'));  // the link's folder, below the root
    for (std::size_t at = 0; at <= target.size();) {
        const std::size_t slash = std::min(target.find('/', at), target.size());
        const std::string_view part = target.substr(at, slash - at);
        if (part == "..") {
            if (--depth < 0) return false;
        } else if (!part.empty() && part != ".") {
            ++depth;
        }
        at = slash + 1;
    }
    return true;
}

bool make_parents(const std::string& file) {
    for (std::size_t slash = file.find('/', 1); slash != std::string::npos; slash = file.find('/', slash + 1)) {
        const std::string dir = file.substr(0, slash);
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

std::string os_error(std::string_view what, const std::string& path) { return std::format("{} {}: {}", what, path, ::strerror(errno)); }

bool write_all(int fd, std::span<const std::byte> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

/// One entry, into `full`, through a part file.
std::optional<std::string> install_entry(const sq::cart::Archive& a, const sq::cart::Entry& e, const std::string& full,
                                         const std::string& to) {
    if (!make_parents(full)) return os_error("cannot make the folders of", full);
    const std::string part = full + ".sqterm-part";
    const mode_t mode = (e.flags & sq::cart::flag::x) ? 0755 : 0644;
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return os_error("cannot create", part);
    for (std::uint64_t at = 0; at < e.size || at == 0;) {
        const std::uint64_t n = std::min<std::uint64_t>(sq::cart::chunk_size, e.size - at);
        if (n == 0) break;
        auto bytes = a.read_range(e, at, n);
        if (!bytes) {
            ::close(fd);
            ::unlink(part.c_str());
            return e.path + ": " + bytes.error().message;
        }
        if (!write_all(fd, *bytes)) {
            const std::string why = os_error("cannot write", part);
            ::close(fd);
            ::unlink(part.c_str());
            return why;
        }
        at += n;
    }
    ::fchmod(fd, mode);  // as asked, whatever the umask
    if (::close(fd) != 0) return os_error("cannot write", part);
    // Another app's folder (sqIDE's): sqterm's, in what the file keeps, made its.
    if (!to.empty()) {
        std::array<char, 512> why{};
        if (sqterm::relocate_file(part.c_str(), to.c_str(), why.data(), why.size()) != 0) {
            ::unlink(part.c_str());
            return std::string(why.data());
        }
    }
    if (::rename(part.c_str(), full.c_str()) != 0) {
        const std::string why = os_error("cannot put in place", full);
        ::unlink(part.c_str());
        return why;
    }
    return std::nullopt;
}

std::optional<std::string> unpack(const char* archive, const char* prefix, const char* meta) {
    auto a = sq::cart::Archive::open(archive);
    if (!a) return a.error().message;
    if (a->manifest().kind != "package") return std::format("a cartridge of kind \"{}\", not a package", a->manifest().kind);
    auto live = a->entries();
    if (!live) return (live.error().entry ? *live.error().entry + ": " : std::string{}) + live.error().message;
    auto manifest = sq::data::parse_json(a->manifest().json, {.ijson = true});
    if (!manifest) return "SQ-INF/manifest.json: " + manifest.error.message;
    const sq::data::JsonValue* consumers = manifest.value.find("consumers");
    const sq::data::JsonValue* sqterm = consumers ? consumers->find("sqterm") : nullptr;
    const sq::data::JsonValue* links = sqterm ? sqterm->find("links") : nullptr;
    if (links && !links->object_if()) return "SQ-INF/manifest.json: consumers.sqterm.links is not an object";
    // Check every link before anything is written.
    if (links) {
        for (const auto& [path, target] : *links->object_if()) {
            if (!plain_path(path) || path.starts_with("SQ-INF/")) return std::format("a link outside the prefix: {}", path);
            if (!target.string_if() || !target_inside(path, *target.string_if())) return std::format("{}: a link to outside the prefix", path);
        }
    }
    const std::string root(prefix);
    std::array<char, 4096> relocated{};
    const std::string to = sqterm::relocation_of(prefix, relocated.data(), relocated.size()) != 0
                               ? std::string(relocated.data())
                               : std::string();
    for (const sq::cart::Entry& e : *live) {
        if (e.path.starts_with("SQ-INF/")) continue;
        if (!plain_path(e.path)) return std::format("a file outside the prefix: {}", e.path);
        if (auto why = install_entry(*a, e, root + "/" + e.path, to)) return why;
    }
    if (links) {
        for (const auto& [path, target] : *links->object_if()) {
            const std::string full = root + "/" + path;
            if (!make_parents(full)) return os_error("cannot make the folders of", full);
            struct stat st{};
            if (::lstat(full.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) return std::format("{}: a folder where the package has a link", path);
                ::unlink(full.c_str());
            }
            if (::symlink(target.string_if()->c_str(), full.c_str()) != 0) return os_error("cannot link", full);
        }
    }
    if (meta != nullptr && *meta != '\0') {
        const std::string out = std::string(meta) + "/manifest.json";
        if (!make_parents(out) || (::mkdir(meta, 0755) != 0 && errno != EEXIST)) return os_error("cannot make", meta);
        const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return os_error("cannot create", out);
        const bool written = write_all(fd, std::as_bytes(std::span(a->manifest().json)));
        if (::close(fd) != 0 || !written) return os_error("cannot write", out);
    }
    return std::nullopt;
}

}  // namespace

namespace sqterm {

int unpack_cartridge(const char* archive, const char* prefix, const char* meta, char* error, unsigned long error_size) {
    const auto why = unpack(archive, prefix, meta);
    if (!why) return 0;
    if (error != nullptr && error_size > 0) {
        const std::size_t n = std::min<std::size_t>(why->size(), error_size - 1);
        std::memcpy(error, why->data(), n);
        error[n] = '\0';
    }
    return 1;
}

}  // namespace sqterm
