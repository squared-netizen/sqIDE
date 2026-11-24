// sqterm: unpacking a package (12.5). Shared by the app (prefix.cpp, at
// first start) and pkg (sq_app/pkg/pkg.cpp, in the shell).
//
// This file is YOURS (ownership class: seeded, sqterm's own).
//
// The reader takes what packages/build.sh writes (ustar: regular files,
// directories, symbolic and hard links; names under 255 bytes), compressed
// with zstd or plain (what pkg backup writes), and nothing that would leave
// the prefix (an absolute name, a ".."). SQ-INF/, the package's manifest,
// goes to a directory of the caller's choosing.
//
// Packages are built for sqterm's folder (built_prefix), and some keep it:
// zsh's /etc/zshrc, git's exec path, tic's terminfo, #! lines. Unpacked into
// another app's folder (sqIDE's), each file is rewritten to that folder as it
// lands (relocate): text whole, a program in place, each C string holding
// the folder ended early with NULs, so nothing in the program moves. That
// needs the new folder no longer than sqterm's (sqIDE's is one shorter), and
// strings not sharing bytes, which lld does only with -O2 (packages link
// without it).
//
// Memory, while it unpacks: zstd's decoder (about 100 KB) and its window
// (packages/build.sh compresses with a 1 MB window), 64 KB in and 128 KB out,
// freed when it is done. Nothing after.
//
// Standard headers, no modules: pkg is built without squared. The interface
// (unpack.hpp) names no standard type, so the app's files, which import std,
// can include it.

#include "unpack.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "zstd.h"

namespace sqterm {

namespace {

/// The largest window the archives may ask for (2^21 bytes, 2 MB): what the
/// decoder allocates is bounded, whatever a damaged archive says.
constexpr int window_log_max = 21;

constexpr std::size_t in_bytes = std::size_t{64} * 1024;
constexpr std::size_t out_bytes = std::size_t{128} * 1024;

[[nodiscard]] std::string errno_text(std::string_view what, std::string_view path) {
    std::string out(what);
    out += ' ';
    out += path;
    out += ": ";
    out += strerror(errno);
    return out;
}

/// `text` with every `from` replaced by `to`.
[[nodiscard]] std::string replaced(std::string_view text, std::string_view from,
                                   std::string_view to) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0;;) {
        const std::size_t hit = text.find(from, at);
        if (hit == std::string_view::npos) {
            out += text.substr(at);
            return out;
        }
        out += text.substr(at, hit - at);
        out += to;
        at = hit + from.size();
    }
}

/// The file's canonical path, or nothing (not there, or not ours to read).
[[nodiscard]] std::string real(const std::string& path) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (resolved == nullptr) {
        return {};
    }
    std::string out(resolved);
    ::free(resolved);
    return out;
}

/// Which folder a path names: its nearest existing folder's device and
/// inode, and the rest of the path below that as it is. A prefix not made
/// yet (the first start) compares too; and two names of one folder compare
/// equal whether one is a symbolic link to the other or both are mounts of
/// it (Android 11's app data isolation mounts /data/user/0/<app> and
/// /data/data/<app> each on its own: realpath tells them apart, stat does not).
struct Place {
    bool found = false;
    dev_t device = 0;
    ino_t inode = 0;
    std::string rest;
    [[nodiscard]] bool operator==(const Place&) const = default;
};

[[nodiscard]] Place place_of(std::string path) {
    std::string rest;
    while (!path.empty()) {
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) {
            return {.found = true, .device = st.st_dev, .inode = st.st_ino, .rest = rest};
        }
        const std::size_t slash = path.rfind('/');
        if (slash == std::string::npos) {
            break;
        }
        rest = path.substr(slash) + rest;
        path.erase(slash);
    }
    return {};
}

[[nodiscard]] bool same_place(const std::string& a, const std::string& b) {
    const Place here = place_of(a);
    return here.found && here == place_of(b);
}

[[nodiscard]] std::string relocate_target(std::string_view prefix) {
    std::string p(prefix);
    while (p.size() > 1 && p.back() == '/') {
        p.pop_back();
    }
    if (p == built_prefix) {
        return {};
    }
    // Android hands an app /data/user/0/<app>/files: the same folder as
    // /data/data/<app>/files for the phone's owner, which is shorter.
    constexpr std::string_view user0 = "/data/user/0/";
    if (p.starts_with(user0)) {
        const std::string data = "/data/data/" + p.substr(user0.size());
        if (same_place(data, p)) {
            p = data;
        }
    }
    if (p == built_prefix || same_place(p, built_prefix)) {
        return {};  // sqterm's folder by another name
    }
    return p;
}

[[nodiscard]] std::string relocate(const std::string& path, std::string_view to) {
    const std::string_view from = built_prefix;
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return errno_text("cannot open", path);
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
        ::close(fd);
        return {};
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        const std::string why = errno_text("cannot map", path);
        ::close(fd);
        return why;
    }
    char* const data = static_cast<char*>(map);
    const std::string_view all(data, size);
    std::string error;
    if (all.find(from) == std::string_view::npos) {
        // Most files: nothing to do.
    } else if (all.substr(0, 8192).find('\0') != std::string_view::npos) {
        // A binary: each C string holding the prefix, rewritten in place.
        if (to.size() > from.size()) {
            error =
                path + ": cannot relocate a program to a longer folder (" + std::string(to) + ")";
        }
        for (std::size_t at = all.find(from); error.empty() && at != std::string_view::npos;) {
            std::size_t end = all.find('\0', at);
            if (end == std::string_view::npos) {
                end = size;
            }
            const std::string now = replaced(all.substr(at, end - at), from, to);
            std::memcpy(data + at, now.data(), now.size());
            std::memset(data + at + now.size(), 0, end - at - now.size());
            at = all.find(from, end);
        }
        if (error.empty() && ::msync(map, size, MS_SYNC) != 0) {
            error = errno_text("cannot write", path);
        }
    } else {
        // Text (a script, a #! line, a .pc, a .cfg): rewritten whole.
        const std::string now = replaced(all, from, to);
        const std::string part = path + ".relocating";
        const int out =
            ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777U);
        bool ok = out >= 0;
        for (std::size_t done = 0; ok && done < now.size();) {
            const ssize_t n = ::write(out, now.data() + done, now.size() - done);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            ok = n > 0;
            done += ok ? static_cast<std::size_t>(n) : 0;
        }
        if (out >= 0) {
            ok = ::fchmod(out, st.st_mode & 07777U) == 0 && ::close(out) == 0 && ok;
        }
        if (!ok || ::rename(part.c_str(), path.c_str()) != 0) {
            error = errno_text("cannot rewrite", path);
            static_cast<void>(::unlink(part.c_str()));
        }
    }
    ::munmap(map, size);
    ::close(fd);
    return error;
}

/// `path`'s parents, made as needed (0755).
[[nodiscard]] bool make_parents(std::string& path) {
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
        path[slash] = '\0';
        const bool made = mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
        path[slash] = '/';
        if (!made) {
            return false;
        }
    }
    return true;
}

/// An octal field of a tar header: digits, ended by a space or a NUL.
[[nodiscard]] std::optional<std::uint64_t> octal(std::span<const char> field) noexcept {
    std::uint64_t value = 0;
    bool digits = false;
    for (const char c : field) {
        if (c >= '0' && c <= '7') {
            value = (value * 8) + static_cast<std::uint64_t>(c - '0');
            digits = true;
        } else if (c == ' ' || c == '\0') {
            if (digits) {
                break;
            }
        } else {
            return std::nullopt;
        }
    }
    return value;
}

/// A NUL-ended (or full) field as text.
[[nodiscard]] std::string_view field_text(std::span<const char> field) noexcept {
    const auto end = std::find(field.begin(), field.end(), '\0');
    return {field.data(), static_cast<std::size_t>(end - field.begin())};
}

/// A name the archive may give: relative, no "..", no empty part.
[[nodiscard]] bool safe_name(std::string_view name) noexcept {
    if (name.empty() || name.front() == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start < name.size()) {
        std::size_t end = name.find('/', start);
        if (end == std::string_view::npos) {
            end = name.size();
        }
        const std::string_view part = name.substr(start, end - start);
        if (part == ".." || (part.empty() && end != name.size())) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

/// The tar reader: bytes in, files out under `root`. A header block (512
/// bytes), then the entry's data, padded to 512.
class TarWriter {
public:
    TarWriter(std::string_view root, std::string_view meta)
        : root_(root), meta_(meta), to_(relocate_target(root)) {}
    TarWriter(const TarWriter&) = delete;
    TarWriter& operator=(const TarWriter&) = delete;
    TarWriter(TarWriter&&) = delete;
    TarWriter& operator=(TarWriter&&) = delete;
    ~TarWriter() { close_file(); }

    /// Bytes of the archive, in order. False, with `error()`, on a fault.
    bool feed(std::span<const std::byte> bytes) {
        while (!bytes.empty() && error_.empty()) {
            if (data_left_ > 0 || pad_left_ > 0) {
                bytes = take_data(bytes);
                continue;
            }
            const std::size_t room = header_.size() - header_used_;
            const std::size_t take = std::min(room, bytes.size());
            std::memcpy(header_.data() + header_used_, bytes.data(), take);
            header_used_ += take;
            bytes = bytes.subspan(take);
            if (header_used_ == header_.size()) {
                header_used_ = 0;
                start_entry();
            }
        }
        return error_.empty();
    }

    /// The archive ended: false if it ended inside an entry.
    [[nodiscard]] bool finish() {
        if (error_.empty() && (data_left_ > 0 || header_used_ != 0)) {
            error_ = "the archive ends inside an entry";
        }
        close_file();
        return error_.empty();
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t files() const noexcept { return files_; }

private:
    std::span<const std::byte> take_data(std::span<const std::byte> bytes) {
        if (data_left_ > 0) {
            const auto take =
                static_cast<std::size_t>(std::min<std::uint64_t>(data_left_, bytes.size()));
            if (fd_ >= 0 && !write_all(bytes.first(take))) {
                error_ = errno_text("cannot write", path_);
            }
            data_left_ -= take;
            if (data_left_ == 0) {
                close_file();
            }
            return bytes.subspan(take);
        }
        const auto take =
            static_cast<std::size_t>(std::min<std::uint64_t>(pad_left_, bytes.size()));
        pad_left_ -= take;
        return bytes.subspan(take);
    }

    [[nodiscard]] bool write_all(std::span<const std::byte> bytes) const {
        while (!bytes.empty()) {
            const ssize_t n = ::write(fd_, bytes.data(), bytes.size());
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        }
        return true;
    }

    void close_file() {
        if (fd_ >= 0) {
            if (::close(fd_) != 0 && error_.empty()) {
                error_ = errno_text("cannot write", path_);
            }
            fd_ = -1;
            // Another app's folder: sqterm's, in what the file keeps, made its.
            if (relocating_ && error_.empty() && data_left_ == 0) {
                error_ = relocate(path_, to_);
            }
            relocating_ = false;
        }
    }

    void start_entry() {
        const std::span<const char> h(header_.data(), header_.size());
        if (std::all_of(h.begin(), h.end(), [](char c) { return c == '\0'; })) {
            return;  // the two empty blocks that end an archive
        }
        // ustar: name[100] mode[8] uid gid size[12] mtime chksum type link[100]
        // "ustar" ... prefix[155] at 345.
        if (field_text(h.subspan(257, 6)).substr(0, 5) != "ustar") {
            error_ = "not a ustar archive";
            return;
        }
        std::string name(field_text(h.subspan(345, 155)));
        if (!name.empty()) {
            name += '/';
        }
        name += field_text(h.subspan(0, 100));
        while (name.size() > 1 && name.back() == '/') {
            name.pop_back();
        }
        if (name.starts_with("./")) {
            name.erase(0, 2);
        }
        const auto mode = octal(h.subspan(100, 8));
        const auto size = octal(h.subspan(124, 12));
        const char type = h[156];
        const std::string_view link = field_text(h.subspan(157, 100));
        if (!mode || !size) {
            error_ = "a damaged header";
            return;
        }
        if (name == "." || name.empty()) {
            return;
        }
        if (!safe_name(name)) {
            error_ = "a name outside the prefix: " + name;
            return;
        }
        // SQ-INF/ (the package's manifest) goes to `meta`, when given:
        // packages would otherwise write over each other's.
        if (!meta_.empty() && (name == "SQ-INF" || name.starts_with("SQ-INF/"))) {
            if (name == "SQ-INF") {
                return;
            }
            path_ = meta_ + "/" + name.substr(7);
        } else {
            path_ = root_ + "/" + name;
        }
        data_left_ = (type == '0' || type == '\0') ? *size : 0;
        const std::uint64_t padded = (*size + 511) / 512 * 512;
        pad_left_ = (type == '0' || type == '\0') ? padded - *size : padded;
        if (!make_parents(path_)) {
            error_ = errno_text("cannot make the directories of", path_);
            return;
        }
        const auto permissions = static_cast<mode_t>(*mode & 07777U);
        switch (type) {
            case '0':
            case '\0':
                // Unlinked first: a running program cannot be written over
                // (ETXTBSY), and a new file takes the new mode.
                static_cast<void>(::unlink(path_.c_str()));
                fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, permissions);
                if (fd_ < 0) {
                    error_ = errno_text("cannot create", path_);
                    return;
                }
                relocating_ = !to_.empty() &&
                              !path_.starts_with(meta_.empty() ? root_ + "/SQ-INF/" : meta_ + "/");
                ++files_;
                if (data_left_ == 0) {
                    close_file();
                }
                return;
            case '5':
                if (::mkdir(path_.c_str(), permissions | 0700) != 0 && errno != EEXIST) {
                    error_ = errno_text("cannot make", path_);
                }
                return;
            case '2':
                static_cast<void>(::unlink(path_.c_str()));
                if (::symlink((to_.empty() ? std::string(link) : replaced(link, built_prefix, to_))
                                  .c_str(),
                              path_.c_str()) != 0) {
                    error_ = errno_text("cannot link", path_);
                }
                return;
            case '1': {
                // A hard link: a copy of the file it names (Android may
                // refuse the link itself).
                if (!safe_name(link)) {
                    error_ = "a link outside the prefix: " + std::string(link);
                    return;
                }
                const std::string from = root_ + "/" + std::string(link);
                if (!copy_file(from, path_, permissions)) {
                    error_ = errno_text("cannot copy to", path_);
                }
                return;
            }
            default:
                return;  // anything else (a device, a fifo) is not unpacked
        }
    }

    [[nodiscard]] static bool copy_file(const std::string& from, const std::string& to,
                                        mode_t permissions) {
        const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            return false;
        }
        static_cast<void>(::unlink(to.c_str()));
        const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, permissions);
        bool ok = out >= 0;
        std::array<char, 16384> buffer{};
        while (ok) {
            const ssize_t n = ::read(in, buffer.data(), buffer.size());
            if (n == 0) {
                break;
            }
            ok = n > 0 && ::write(out, buffer.data(), static_cast<std::size_t>(n)) == n;
        }
        ::close(in);
        if (out >= 0) {
            ok = ::close(out) == 0 && ok;
        }
        return ok;
    }

    std::string root_;
    std::string meta_;
    std::string to_;  // the folder to relocate to; empty: sqterm's own
    bool relocating_ = false;
    std::string path_;
    std::array<char, 512> header_{};
    std::size_t header_used_ = 0;
    std::uint64_t data_left_ = 0;
    std::uint64_t pad_left_ = 0;
    std::size_t files_ = 0;
    int fd_ = -1;
    std::string error_;
};

}  // namespace

std::string unpack(std::string_view archive, std::string_view prefix, std::string_view meta) {
    const std::string path(archive);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno_text("cannot open", path);
    }
    ZSTD_DStream* stream = ZSTD_createDStream();
    if (stream == nullptr) {
        ::close(fd);
        return "no memory for the decoder";
    }
    static_cast<void>(ZSTD_DCtx_setParameter(stream, ZSTD_d_windowLogMax, window_log_max));
    const auto in_buffer = std::make_unique_for_overwrite<std::byte[]>(in_bytes);
    const auto out_buffer = std::make_unique_for_overwrite<std::byte[]>(out_bytes);
    TarWriter tar(prefix, meta);
    // zstd's magic (28 b5 2f fd) or a plain tar (pkg backup writes one).
    bool plain = false;
    {
        std::array<unsigned char, 4> magic{};
        plain = ::pread(fd, magic.data(), magic.size(), 0) == 4 &&
                magic != std::array<unsigned char, 4>{0x28, 0xb5, 0x2f, 0xfd};
    }
    std::string error;
    std::size_t last = 1;  // what the decoder said it wants: 0 at a frame's end
    for (;;) {
        const ssize_t got = ::read(fd, in_buffer.get(), in_bytes);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errno_text("cannot read", path);
            break;
        }
        if (got == 0) {
            if (last != 0) {
                error = "the archive is cut short";
            }
            break;
        }
        if (plain) {
            if (!tar.feed({in_buffer.get(), static_cast<std::size_t>(got)})) {
                error = tar.error();
                break;
            }
            last = 0;
            continue;
        }
        ZSTD_inBuffer in{in_buffer.get(), static_cast<std::size_t>(got), 0};
        // Until the input is used and the output did not fill: a full
        // output may mean more is waiting in the decoder.
        bool full = true;
        while (error.empty() && (in.pos < in.size || full)) {
            ZSTD_outBuffer out{out_buffer.get(), out_bytes, 0};
            last = ZSTD_decompressStream(stream, &out, &in);
            if (ZSTD_isError(last) != 0U) {
                error = "the archive is damaged (zstd error " +
                        std::to_string(static_cast<int>(ZSTD_getErrorCode(last))) + ")";
                break;
            }
            full = out.pos == out_bytes;
            if (!tar.feed({out_buffer.get(), out.pos})) {
                error = tar.error();
            }
        }
        if (!error.empty()) {
            break;
        }
    }
    ZSTD_freeDStream(stream);
    ::close(fd);
    if (!tar.finish() && error.empty()) {
        error = tar.error();
    }
    return error;
}

int relocation_of(const char* prefix, char* out, unsigned long out_size) {
    const std::string to = relocate_target(prefix != nullptr ? prefix : "");
    if (out != nullptr && out_size > 0) {
        std::snprintf(out, out_size, "%s", to.c_str());
    }
    return to.empty() ? 0 : 1;
}

int relocate_file(const char* path, const char* to, char* error, unsigned long error_size) {
    const std::string failed = relocate(path, to);
    if (error != nullptr && error_size > 0) {
        std::snprintf(error, error_size, "%s", failed.c_str());
    }
    return failed.empty() ? 0 : 1;
}

int unpack_archive(const char* archive, const char* prefix, const char* meta, char* error,
                   unsigned long error_size) {
    const std::string failed = unpack(archive, prefix, meta != nullptr ? meta : "");
    if (error != nullptr && error_size > 0) {
        std::snprintf(error, error_size, "%s", failed.c_str());
    }
    return failed.empty() ? 0 : 1;
}

}  // namespace sqterm
