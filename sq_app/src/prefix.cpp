// sqterm: the prefix (12.5). The packages the APK carries, unpacked into the
// app's files (files/usr), where the shell and its programs run from.
//
// This file is YOURS (ownership class: seeded, sqterm's own).
//
// Two halves, on two threads:
//
//   stage_packages   the UI thread, as the app starts (squared_main): the APK's
//                    package index (assets/packages/index, written by
//                    packages/build.sh) against the one last installed
//                    (usr/var/lib/sqterm/packages). When they differ, each
//                    archive is copied out of the APK into usr/var/cache/sqterm/,
//                    a plain file the model's thread can read whatever the
//                    activity does meanwhile. Nothing to do: nothing read but
//                    the two indexes.
//   unpack_package   the model's thread (Shell::start), before the shell: one
//                    archive unpacked into the prefix (unpack.cpp, shared with
//                    pkg), its manifest into usr/var/lib/sqterm/pkg/<name>/.
//                    The Shell then records the index as installed and
//                    removes the copies.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sqterm.hpp"
#include "unpack.hpp"

namespace sqterm {

namespace {

constexpr std::string_view index_asset = "packages/index";
constexpr std::string_view index_header = "sqterm packages 1\n";
constexpr std::string_view installed_file = "usr/var/lib/sqterm/packages";
constexpr std::string_view cache_directory = "usr/var/cache/sqterm";
/// Under the prefix: each package's manifest, <name>/manifest.json (pkg's).
constexpr std::string_view pkg_directory = "var/lib/sqterm/pkg";

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
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

}  // namespace

void stage_packages(sq::files::FileSystem& files, Places& places) {
    using sq::files::FileType;
    places.staged.clear();
    places.index.clear();
    const sq::files::FileReadResult index = files.read(FileType::Internal, index_asset);
    if (!index) {
        return;  // a build with no packages (packages/build.sh was not run)
    }
    const std::string_view text = as_text(index.bytes);
    if (!text.starts_with(index_header)) {
        std::fprintf(stderr, "sqterm: %.*s is not a package index\n",
                     static_cast<int>(index_asset.size()), index_asset.data());
        return;
    }
    if (const sq::files::FileReadResult installed = files.read(FileType::Local, installed_file);
        installed && as_text(installed.bytes) == text) {
        return;  // installed already: nothing copied
    }
    static_cast<void>(files.make_directories(FileType::Local, cache_directory));
    // Each line after the first: "name version file sha".
    std::size_t at = index_header.size();
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(at, end - at);
        at = end + 1;
        std::array<std::string_view, 4> word{};
        std::size_t words = 0;
        for (std::size_t from = 0; from < line.size() && words < word.size();) {
            const std::size_t space = std::min(line.find(' ', from), line.size());
            word[words++] = line.substr(from, space - from);
            from = space + 1;
        }
        if (words < 3) {
            continue;
        }
        const std::string asset = "packages/" + std::string(word[2]);
        const std::string copy = std::string(cache_directory) + "/" + std::string(word[2]);
        // Stored uncompressed in the APK (SQ_STORED has zst): mapped, then written out.
        sq::files::MapResult mapped = files.map(FileType::Internal, asset);
        sq::files::FileError wrote;
        if (mapped) {
            wrote = files.write(FileType::Local, copy, mapped.file.bytes(), false);
        } else {
            const sq::files::FileReadResult read = files.read(FileType::Internal, asset);
            if (!read) {
                std::fprintf(stderr, "sqterm: %s is not in the APK\n", asset.c_str());
                continue;
            }
            wrote = files.write(FileType::Local, copy, read.bytes, false);
        }
        if (wrote) {
            std::fprintf(stderr, "sqterm: cannot copy %s: %s\n", asset.c_str(),
                         wrote.message.c_str());
            continue;
        }
        places.staged.push_back({.name = std::string(word[0]) + " " + std::string(word[1]),
                                 .archive = files.resolve_platform_path(FileType::Local, copy)});
    }
    places.index = std::string(text);
    places.installed = files.resolve_platform_path(FileType::Local, installed_file);
}

std::string unpack_package(std::string_view archive, std::string_view prefix,
                           std::string_view name) {
    // The manifest (SQ-INF/) goes to usr/var/lib/sqterm/pkg/<name>/, where
    // pkg finds it.
    const std::string meta = std::string(prefix) + "/" + std::string(pkg_directory) + "/" +
                             std::string(name);
    std::array<char, 512> error{};
    const int failed = unpack_archive(std::string(archive).c_str(), std::string(prefix).c_str(),
                                      meta.c_str(), error.data(), error.size());
    return failed == 0 ? std::string() : std::string(error.data());
}

bool record_installed(const Places& places) {
    if (places.installed.empty()) {
        return false;
    }
    std::string path = places.installed;
    if (!make_parents(path)) {
        return false;
    }
    const std::string fresh = path + ".new";
    const int fd = ::open(fresh.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const bool wrote = ::write(fd, places.index.data(), places.index.size()) ==
                       static_cast<ssize_t>(places.index.size());
    const bool closed = ::close(fd) == 0;
    return wrote && closed && ::rename(fresh.c_str(), path.c_str()) == 0;
}

}  // namespace sqterm
