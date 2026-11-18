// pkg: sqterm's packages, in the shell (12.5).
//
// This file is YOURS (ownership class: seeded, sqterm's own).
//
//   pkg list                         what is installed: name, version
//   pkg files NAME                   the files a package installed
//   pkg install NAME [--consume]     the newest NAME-<version>-*.tar.zst (or
//                                    .tar) in the download folder ($PKG_DIR,
//                                    default /sdcard/Download): what
//                                    fetch_packages.sh --get leaves there
//   pkg install FILE [--consume]     a package from a file (.tar.zst, or the
//                                    .tar pkg backup writes); --consume
//                                    deletes FILE once it is installed
//   pkg remove NAME                  its files, then its record
//   pkg backup NAME [DIR]            its installed files as a package again,
//                                    DIR/NAME-VERSION.tar (default: here)
//
// Offline: pkg reads files, never the network. Getting a package file is
// its own step (tools/fetch_packages.sh in Termux, or any copy).
//
// Each package carries SQ-INF/manifest.json (packages/build.sh writes it: the
// squared-cartridge envelope, kind "package", and a "sqterm" consumer with
// its key and its files). Installed, it is kept as
// $PREFIX/var/lib/sqterm/pkg/NAME/manifest.json: what `files`, `remove` and
// `backup` read, and what an upgrade compares, so the files a new version no
// longer has are deleted. The app's first start writes the same records for
// the packages in its APK.
//
// Built as a package of its own (packages/pkg), with the archive reader the
// app uses (sq_app/src/unpack.cpp) and squared's yyjson. Standard headers,
// no squared: it is a small program in the prefix.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "unpack.hpp"
#include "yyjson.h"

namespace {

struct Manifest {
    std::string id;
    std::string version;
    std::vector<std::string> files;  ///< relative to the prefix
};

std::string prefix;      // $PREFIX
std::string records;     // $PREFIX/var/lib/sqterm/pkg

int fail(const std::string& message) {
    std::fprintf(stderr, "pkg: %s\n", message.c_str());
    return 1;
}

std::string errno_text(std::string_view what, const std::string& path) {
    return std::string(what) + " " + path + ": " + strerror(errno);
}

/// Whether a path is in shared storage (Download and the rest), where an
/// app reads and writes only with the Files permission.
bool shared_storage(const std::string& path) {
    std::array<char, PATH_MAX> real{};
    const std::string resolved = ::realpath(path.c_str(), real.data()) != nullptr ? real.data() : path;
    for (const std::string_view root : {"/sdcard", "/storage", "/mnt/sdcard"}) {
        if (resolved.starts_with(root) || path.starts_with(root)) return true;
    }
    return false;
}

/// What to do when Android refused shared storage: said once, after the error.
constexpr std::string_view files_permission =
    "\n     sqterm needs the Files permission: Settings, Apps, sqterm, Permissions, Files";

/// A package's name: letters, digits, '.', '_', '+', '-'; not starting with '.'.
bool good_name(std::string_view name) {
    return !name.empty() && name.front() != '.' &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '+' || c == '-';
           });
}

/// A path a manifest may name: relative, no "..", no empty part.
bool good_path(std::string_view path) {
    if (path.empty() || path.front() == '/') return false;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = std::min(path.find('/', at), path.size());
        const std::string_view part = path.substr(at, slash - at);
        if (part.empty() || part == "." || part == "..") return false;
        at = slash + 1;
    }
    return true;
}

std::optional<Manifest> read_manifest(const std::string& file, std::string& error) {
    yyjson_read_err read_error{};
    yyjson_doc* doc = yyjson_read_file(file.c_str(), 0, nullptr, &read_error);
    if (doc == nullptr) {
        error = file + ": " + read_error.msg;
        return std::nullopt;
    }
    Manifest m;
    yyjson_val* root = yyjson_doc_get_root(doc);
    const char* format = yyjson_get_str(yyjson_obj_get(root, "format"));
    const char* kind = yyjson_get_str(yyjson_obj_get(root, "kind"));
    const char* id = yyjson_get_str(yyjson_obj_get(root, "id"));
    const char* version = yyjson_get_str(yyjson_obj_get(root, "version"));
    yyjson_val* files =
        yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "consumers"), "sqterm"), "files");
    if (format == nullptr || std::string_view(format) != "squared-cartridge" || kind == nullptr ||
        std::string_view(kind) != "package" || id == nullptr || version == nullptr ||
        !yyjson_is_arr(files)) {
        error = file + ": not a squared-cartridge manifest of kind package";
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    m.id = id;
    m.version = version;
    std::size_t index = 0, count = 0;
    yyjson_val* item = nullptr;
    yyjson_arr_foreach(files, index, count, item) {
        const char* path = yyjson_get_str(item);
        if (path == nullptr || !good_path(path)) {
            error = file + ": a file outside the prefix";
            yyjson_doc_free(doc);
            return std::nullopt;
        }
        m.files.emplace_back(path);
    }
    yyjson_doc_free(doc);
    if (!good_name(m.id)) {
        error = file + ": a bad package name: " + m.id;
        return std::nullopt;
    }
    return m;
}

std::string record_of(std::string_view name) { return records + "/" + std::string(name); }

std::optional<Manifest> installed(std::string_view name, std::string& error) {
    if (!good_name(name)) {
        error = "a bad package name: " + std::string(name);
        return std::nullopt;
    }
    const std::string file = record_of(name) + "/manifest.json";
    if (::access(file.c_str(), F_OK) != 0) {
        error = std::string(name) + " is not installed";
        return std::nullopt;
    }
    return read_manifest(file, error);
}

int remove_entry(const char* path, const struct stat*, int, struct FTW*) { return ::remove(path); }

bool remove_tree(const std::string& path) {
    return ::nftw(path.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0 || errno == ENOENT;
}

bool make_directories(const std::string& path) {
    for (std::size_t slash = path.find('/', 1);; slash = path.find('/', slash + 1)) {
        const std::string part = path.substr(0, slash);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        if (slash == std::string::npos) return true;
    }
}

/// Delete `files` (relative to the prefix), then their directories that are
/// left empty, up to the prefix. Directories themselves are never listed.
void delete_files(const std::vector<std::string>& files) {
    std::set<std::string> parents;
    for (const std::string& file : files) {
        const std::string path = prefix + "/" + file;
        struct stat st {};
        if (::lstat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
            static_cast<void>(::unlink(path.c_str()));
        }
        for (std::size_t slash = file.rfind('/'); slash != std::string::npos && slash > 0;
             slash = file.rfind('/', slash - 1)) {
            parents.insert(file.substr(0, slash));
        }
    }
    // Deepest first: a longer path sorts after its parent, so in reverse.
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
        static_cast<void>(::rmdir((prefix + "/" + *it).c_str()));  // only if empty
    }
}

int cmd_list() {
    DIR* dir = ::opendir(records.c_str());
    if (dir == nullptr) return 0;  // nothing installed yet
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(dir)) {
        if (good_name(entry->d_name)) names.emplace_back(entry->d_name);
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    // Read first, so the version column lines up past the longest name
    // (zsh-syntax-highlighting).
    std::vector<std::pair<std::string, std::string>> rows;
    std::size_t width = 0;
    for (const std::string& name : names) {
        std::string error;
        if (const auto m = installed(name, error)) {
            width = std::max(width, m->id.size());
            rows.emplace_back(m->id, m->version);
        } else {
            std::fprintf(stderr, "pkg: %s\n", error.c_str());
        }
    }
    for (const auto& [id, version] : rows) {
        std::printf("%-*s  %s\n", static_cast<int>(width), id.c_str(), version.c_str());
    }
    return 0;
}

int cmd_files(std::string_view name) {
    std::string error;
    const auto m = installed(name, error);
    if (!m) return fail(error);
    for (const std::string& file : m->files) std::printf("%s\n", file.c_str());
    return 0;
}

/// The download folder pkg looks in for an install by name.
std::string download_dir() {
    const char* d = std::getenv("PKG_DIR");
    return d != nullptr && *d != '\0' ? d : "/sdcard/Download";
}

/// NAME-<version>[-key].tar.zst or .tar: the version starts with a digit, so
/// zsh never matches zsh-autosuggestions-0.7.1.tar.zst.
bool archive_of(std::string_view file, std::string_view name) {
    if (!file.starts_with(name) || file.size() <= name.size() + 1 || file[name.size()] != '-') return false;
    const char next = file[name.size() + 1];
    if (next < '0' || next > '9') return false;
    return file.ends_with(".tar.zst") || file.ends_with(".tar");
}

/// What `pkg install NAME` means: the newest NAME archive in the download
/// folder (by time: fetch_packages.sh --get writes the one it fetched last).
/// Empty, with the reason in error, when there is none.
std::string find_archive(const std::string& name, std::string& error) {
    const std::string dir = download_dir();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        const bool refused = errno == EACCES || errno == EPERM;
        error = errno_text("cannot read", dir);
        if (refused && shared_storage(dir)) error += files_permission;
        return {};
    }
    std::string best;
    std::vector<std::string> others;
    struct timespec best_time{};
    while (const dirent* e = ::readdir(d)) {
        if (!archive_of(e->d_name, name)) continue;
        const std::string path = dir + "/" + e->d_name;
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        const bool newer = best.empty() || st.st_mtim.tv_sec > best_time.tv_sec ||
                           (st.st_mtim.tv_sec == best_time.tv_sec && st.st_mtim.tv_nsec > best_time.tv_nsec);
        if (newer) {
            if (!best.empty()) others.push_back(best);
            best = path;
            best_time = st.st_mtim;
        } else {
            others.push_back(path);
        }
    }
    ::closedir(d);
    if (best.empty()) {
        error = "no " + name + "-<version>.tar.zst in " + dir +
                "\n     fetch it first, in Termux: tools/fetch_packages.sh --get " + name;
        return {};
    }
    for (const std::string& o : others) std::printf("pkg: also in %s, older: %s\n", dir.c_str(), o.c_str() + dir.size() + 1);
    return best;
}

int cmd_install(const std::string& file, bool consume) {
    if (!make_directories(records)) return fail(errno_text("cannot make", records));
    const std::string incoming = records + "/.incoming";
    remove_tree(incoming);
    std::array<char, 512> error{};
    if (sqterm::unpack_archive(file.c_str(), prefix.c_str(), incoming.c_str(), error.data(),
                       error.size()) != 0) {
        remove_tree(incoming);
        const std::string_view said(error.data());
        if ((said.find("Permission denied") != std::string_view::npos ||
             said.find("Operation not permitted") != std::string_view::npos) &&
            shared_storage(file)) {
            return fail(file + ": " + error.data() + std::string(files_permission));
        }
        return fail(file + ": " + error.data());
    }
    std::string why;
    const auto fresh = read_manifest(incoming + "/manifest.json", why);
    if (!fresh) {
        remove_tree(incoming);
        return fail(why + " (its files are unpacked; no record was kept)");
    }
    // An upgrade: what the old version had and this one has not goes.
    std::string none;
    if (const auto old = installed(fresh->id, none)) {
        const std::set<std::string> keep(fresh->files.begin(), fresh->files.end());
        std::vector<std::string> gone;
        for (const std::string& f : old->files) {
            if (!keep.contains(f)) gone.push_back(f);
        }
        delete_files(gone);
        if (!gone.empty()) {
            std::printf("pkg: %s %s -> %s: %zu old file(s) removed\n", fresh->id.c_str(),
                        old->version.c_str(), fresh->version.c_str(), gone.size());
        }
    }
    const std::string record = record_of(fresh->id);
    remove_tree(record);
    if (::rename(incoming.c_str(), record.c_str()) != 0) {
        return fail(errno_text("cannot record", record));
    }
    std::printf("pkg: %s %s installed (%zu files)\n", fresh->id.c_str(), fresh->version.c_str(),
                fresh->files.size());
    if (consume) {
        if (::unlink(file.c_str()) != 0) return fail(errno_text("installed, but cannot delete", file));
        std::printf("pkg: %s deleted\n", file.c_str());
    }
    return 0;
}

int cmd_remove(std::string_view name) {
    std::string error;
    const auto m = installed(name, error);
    if (!m) return fail(error);
    delete_files(m->files);
    if (!remove_tree(record_of(name))) return fail(errno_text("cannot remove", record_of(name)));
    std::printf("pkg: %s %s removed\n", m->id.c_str(), m->version.c_str());
    return 0;
}

// ---- backup: a plain ustar archive, as packages/build.sh writes them ----

class TarOut {
public:
    explicit TarOut(int fd) : fd_(fd) {}
    bool ok() const { return ok_; }

    void add(const std::string& name, const std::string& path) {
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0) {
            std::fprintf(stderr, "pkg: %s is missing (left out)\n", name.c_str());
            return;
        }
        std::array<char, 512> h{};
        if (!split_name(name, h)) {
            std::fprintf(stderr, "pkg: %s: name too long (left out)\n", name.c_str());
            return;
        }
        number(h, 100, 8, st.st_mode & 07777);
        number(h, 108, 8, 0);
        number(h, 116, 8, 0);
        number(h, 136, 12, 0);  // dated at the epoch, as build.sh packs
        std::memcpy(h.data() + 257, "ustar\0" "00", 8);
        if (S_ISLNK(st.st_mode)) {
            std::array<char, 101> target{};
            const ssize_t n = ::readlink(path.c_str(), target.data(), 100);
            if (n < 0 || n > 100) return bad(path);
            h[156] = '2';
            std::memcpy(h.data() + 157, target.data(), static_cast<std::size_t>(n));
            number(h, 124, 12, 0);
            write_header(h);
            return;
        }
        if (!S_ISREG(st.st_mode)) return;
        h[156] = '0';
        number(h, 124, 12, static_cast<std::uint64_t>(st.st_size));
        write_header(h);
        const int in = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) return bad(path);
        std::array<char, 65536> buffer{};
        std::uint64_t total = 0;
        for (;;) {
            const ssize_t n = ::read(in, buffer.data(), buffer.size());
            if (n <= 0) {
                if (n < 0) bad(path);
                break;
            }
            put(buffer.data(), static_cast<std::size_t>(n));
            total += static_cast<std::uint64_t>(n);
        }
        ::close(in);
        const std::array<char, 512> zero{};
        put(zero.data(), static_cast<std::size_t>((512 - total % 512) % 512));
    }

    void finish() {
        const std::array<char, 1024> zero{};
        put(zero.data(), zero.size());
    }

private:
    static bool split_name(const std::string& name, std::array<char, 512>& h) {
        if (name.size() <= 100) {
            std::memcpy(h.data(), name.data(), name.size());
            return true;
        }
        // ustar: prefix[155] at 345, name[100]; split at a '/'.
        for (std::size_t slash = name.find('/'); slash != std::string::npos;
             slash = name.find('/', slash + 1)) {
            if (slash <= 155 && name.size() - slash - 1 <= 100) {
                std::memcpy(h.data() + 345, name.data(), slash);
                std::memcpy(h.data(), name.data() + slash + 1, name.size() - slash - 1);
                return true;
            }
        }
        return false;
    }
    static void number(std::array<char, 512>& h, std::size_t at, std::size_t size, std::uint64_t v) {
        std::snprintf(h.data() + at, size, "%0*llo", static_cast<int>(size - 1),
                      static_cast<unsigned long long>(v));
    }
    void write_header(std::array<char, 512>& h) {
        std::memset(h.data() + 148, ' ', 8);
        unsigned sum = 0;
        for (const char c : h) sum += static_cast<unsigned char>(c);
        std::snprintf(h.data() + 148, 8, "%06o", sum);
        h[155] = ' ';
        put(h.data(), h.size());
    }
    void put(const char* data, std::size_t size) {
        while (ok_ && size > 0) {
            const ssize_t n = ::write(fd_, data, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ok_ = false;
                return;
            }
            data += n;
            size -= static_cast<std::size_t>(n);
        }
    }
    void bad(const std::string& path) {
        std::fprintf(stderr, "pkg: cannot read %s: %s\n", path.c_str(), strerror(errno));
        ok_ = false;
    }
    int fd_;
    bool ok_ = true;
};

int cmd_backup(std::string_view name, const std::string& dir) {
    std::string error;
    const auto m = installed(name, error);
    if (!m) return fail(error);
    const std::string out = dir + "/" + m->id + "-" + m->version + ".tar";
    const std::string part = out + ".part";
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        const bool refused = errno == EACCES || errno == EPERM;
        std::string message = errno_text("cannot create", part);
        // 2026-11-17, on the phone: "cannot create .../Download/zsh-5.9.2.tar.part:
        // Operation not permitted", with nothing to say why.
        if (refused && shared_storage(dir)) message += files_permission;
        return fail(message);
    }
    TarOut tar(fd);
    tar.add("SQ-INF/manifest.json", record_of(m->id) + "/manifest.json");
    std::vector<std::string> files = m->files;
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) tar.add(f, prefix + "/" + f);
    tar.finish();
    const bool closed = ::close(fd) == 0;
    if (!tar.ok() || !closed || ::rename(part.c_str(), out.c_str()) != 0) {
        ::unlink(part.c_str());
        return fail("cannot write " + out);
    }
    std::printf("pkg: %s\n", out.c_str());
    return 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage: pkg list\n"
                 "       pkg files NAME\n"
                 "       pkg install NAME|FILE [--consume]\n"
                 "       pkg remove NAME\n"
                 "       pkg backup NAME [DIR]\n");
    return 2;
}

/// Levenshtein distance, for a mistyped command.
std::size_t distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

/// A command pkg does not have: say so, and name the one it is closest to
/// (`pkg istall` means install), rather than only print the usage.
int unknown(const std::string& cmd) {
    static constexpr std::string_view commands[] = {"list", "files", "install", "remove", "backup"};
    for (const std::string_view c : commands) {
        if (c == cmd) return usage();  // a known command, wrong arguments
    }
    std::string_view best;
    std::size_t best_distance = 3;  // at most two edits away
    for (const std::string_view c : commands) {
        const std::size_t d = distance(cmd, c);
        if (d < best_distance) best = c, best_distance = d;
    }
    if (best.empty()) {
        std::fprintf(stderr, "pkg: unknown command '%s'\n", cmd.c_str());
    } else {
        std::fprintf(stderr, "pkg: unknown command '%s'; did you mean %.*s?\n", cmd.c_str(),
                     static_cast<int>(best.size()), best.data());
    }
    return usage();
}

}  // namespace

int main(int argc, char** argv) {
    const char* p = std::getenv("PREFIX");
    if (p == nullptr || *p != '/') return fail("PREFIX is not set (sqterm's shell sets it)");
    prefix = p;
    records = prefix + "/var/lib/sqterm/pkg";
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    const std::string& cmd = args[0];
    if (cmd == "list" && args.size() == 1) return cmd_list();
    if (cmd == "files" && args.size() == 2) return cmd_files(args[1]);
    if (cmd == "install" && (args.size() == 2 || args.size() == 3)) {
        // --consume before or after the package.
        const bool consume = args.size() == 3;
        if (consume && args[1] != "--consume" && args[2] != "--consume") return usage();
        const std::string& what = args.size() == 3 && args[1] == "--consume" ? args[2] : args[1];
        // A path, or a file here: as given. A bare package name: the newest
        // archive of it in the download folder.
        struct stat st{};
        if (what.find('/') == std::string::npos && ::stat(what.c_str(), &st) != 0 && good_name(what)) {
            std::string error;
            const std::string found = find_archive(what, error);
            if (found.empty()) return fail(error);
            std::printf("pkg: %s\n", found.c_str());
            return cmd_install(found, consume);
        }
        return cmd_install(what, consume);
    }
    if (cmd == "remove" && args.size() == 2) return cmd_remove(args[1]);
    if (cmd == "backup" && (args.size() == 2 || args.size() == 3)) {
        return cmd_backup(args[1], args.size() == 3 ? args[2] : std::string("."));
    }
    return unknown(cmd);
}
