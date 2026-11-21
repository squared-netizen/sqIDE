// pkg: sqterm's packages, in the shell (12.5).
//
// This file is YOURS (ownership class: seeded, sqterm's own).
//
//   pkg list                         what is installed: name, version, key,
//                                    and the index's key where it differs
//   pkg info NAME                    installed or not: version, key, size,
//                                    licence, the index's, needs, needed by,
//                                    where its archive already is
//   pkg files NAME                   the files a package installed
//   pkg owns PATH|COMMAND            which package installed a file
//                                    (pkg owns clang: bin/clang)
//   pkg update                       settle the index: what there is
//   pkg upgrade [NAME...]            each installed package (or those named)
//                                    whose key differs from the index's, and
//                                    what installed packages need and lack
//   pkg search [WORD]                the index's packages (with WORD in the
//                                    name), what they need, which are installed
//   pkg install NAME... [--consume]  by name: the index's, and what it needs
//                                    first; a name no index knows, the newest
//                                    NAME-<version>-*.sq (or .tar.zst, .tar) in the
//                                    packages folder, then Download
//   pkg install FILE [--consume]     a package from a file (.sq, .tar.zst, or
//                                    the .tar pkg backup writes); --consume
//                                    deletes FILE once it is installed
//   pkg remove NAME [--force]        its files, then its record; refused while
//                                    an installed package needs it
//   pkg backup NAME [DIR]            its installed files as a package again,
//                                    DIR/NAME-VERSION.tar (default: here)
//   pkg clean                        empty the internal archive
//
// --from SOURCE (install, upgrade, update, search) looks in one source
// only: archive (the internal archive, with the index last settled on),
// sqterm (the packages folder and its index) or repo (the release: its
// index, and every archive downloaded afresh). The first two never touch
// the network. Only a plain or --from repo update keeps the index it
// settles on, so a look at one folder never makes the next upgrade go back.
//
// Names, never file names: a package is installed, upgraded and removed by
// its name, wherever its archive is, and the keys and sha256s in archive
// names are pkg's to match, not anyone's to type.
//
// The index. CI publishes, beside the archives in the GitHub release named
// `packages`, an `index`: for each package main's recipes ask for, its
// version, key, whether the APK carries it, its archive's sha256, and what it
// needs at run time (packages/needs). tools/fetch_packages.sh --get writes
// one of the same form into the packages folder for what it fetched. `pkg
// update` (and upgrade, install NAME and search, which start with it)
// settles one: the release's, through curl, with what only the packages
// folder's names after it; offline, the packages folder's; else the last
// one settled on. Kept in $PREFIX/var/lib/sqterm/release-index.
//
// The archives. An indexed package's archive is looked for, by its exact
// name and checked against the index's sha256, in
//   $PREFIX/var/cache/sqterm/archive   the internal archive: what pkg
//                                      downloaded, one a package (pkg clean)
//   $PKG_DIR                           the packages folder, default
//                                      /sdcard/sqterm/packages (shared
//                                      storage: Termux writes it, and it
//                                      survives the app)
//   /sdcard/Download                   a browser's
// and only then downloaded from the release into the internal archive. So
// update and upgrade work offline from the packages folder, and an archive
// already here is never downloaded again. The sha256s guard against a
// broken or mixed-up file, and HTTPS the way there; neither is a signature
// (sqcart 3.0 defines those). SQ_PACKAGES_URL (default
// https://github.com/squared-netizen/sqterm/releases/download) points at a
// mirror, or a test.
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
#include <spawn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "unpack.hpp"
#include "yyjson.h"

extern char** environ;

namespace {

struct Manifest {
    std::string id;
    std::string version;
    std::string key;                 ///< consumers.sqterm.key; empty if none
    std::string license;
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
    yyjson_val* sqterm = yyjson_obj_get(yyjson_obj_get(root, "consumers"), "sqterm");
    yyjson_val* files = yyjson_obj_get(sqterm, "files");
    const char* key = yyjson_get_str(yyjson_obj_get(sqterm, "key"));
    if (format == nullptr || std::string_view(format) != "squared-cartridge" || kind == nullptr ||
        std::string_view(kind) != "package" || id == nullptr || version == nullptr ||
        !yyjson_is_arr(files)) {
        error = file + ": not a squared-cartridge manifest of kind package";
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    m.id = id;
    m.version = version;
    if (key != nullptr) m.key = key;
    if (const char* l = yyjson_get_str(yyjson_obj_get(root, "license"))) m.license = l;
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

// ---- where packages come from: the release, the packages folder, the archive ----

/// One package as an index names it: an archive built from main's recipes.
struct Available {
    std::string name;
    std::string version;
    std::string key;
    std::string sha256;
    bool bundled = true;             ///< in the APK as well
    std::vector<std::string> needs;  ///< what it needs at run time, installed first
    bool cartridge = false;          ///< a squared cartridge (.sq), else a tarball

    std::string asset() const { return name + "-" + version + "-" + key + (cartridge ? ".sq" : ".tar.zst"); }
};

/// Index versions: 1 (no needs), 2 (a sixth field: needs, comma-separated,
/// or -), 3 (the same, its archives cartridges, .sq: the release's index-3;
/// pkg's own merged index marks a tarball with a seventh field, "tar").
constexpr std::string_view index_header_1 = "sqterm release index 1";
constexpr std::string_view index_header_2 = "sqterm release index 2";
constexpr std::string_view index_header_3 = "sqterm release index 3";

/// The index pkg last settled on (pkg update), all sources merged.
std::string index_path() { return prefix + "/var/lib/sqterm/release-index"; }
/// The internal archive: what pkg downloaded, kept (one archive a package) so
/// a reinstall or an install on another day needs no network. pkg clean.
std::string archive_dir() { return prefix + "/var/cache/sqterm/archive"; }
/// The packages folder in shared storage, which Termux can write
/// (tools/fetch_packages.sh --get) and which survives the app: $PKG_DIR.
std::string packages_dir() {
    const char* d = std::getenv("PKG_DIR");
    return d != nullptr && *d != '\0' ? d : "/sdcard/sqterm/packages";
}
/// Where a browser leaves a package downloaded by hand.
constexpr std::string_view download_dir = "/sdcard/Download";

/// --from: where install, upgrade, update and search may look. `any` (no
/// --from) is every source in turn; the others, that one alone:
///   archive  the internal archive, and the index last settled on; no network
///   sqterm   the packages folder ($PKG_DIR) and its index; no network
///   repo     the release (sqterm's GitHub repository): its index, and every
///            archive downloaded afresh, whatever is here
enum class Source { any, archive, sqterm, repo };
Source from = Source::any;

std::string_view source_name(Source s) {
    switch (s) {
        case Source::archive: return "archive";
        case Source::sqterm: return "sqterm";
        case Source::repo: return "repo";
        case Source::any: break;
    }
    return "any";
}

/// Where the release's files are: <base>/<file>.
std::string release_base() {
    const char* url = std::getenv("SQ_PACKAGES_URL");
    if (url != nullptr && *url != '\0') return std::string(url) + "/packages";
    const char* repo = std::getenv("SQ_PACKAGES_REPO");
    return "https://github.com/" + std::string(repo != nullptr && *repo != '\0' ? repo : "squared-netizen/sqterm") +
           "/releases/download/packages";
}

bool hex_of(std::string_view text, std::size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

/// An index as text: its header, then "name version key bundled sha256
/// [needs]" a line, in the order the packages build (what one depends on
/// before it). Empty, with the reason in error, when it is not one.
std::optional<std::vector<Available>> parse_index(const std::string& text, std::string& error) {
    std::vector<Available> out;
    std::size_t at = 0;
    std::size_t fields = 0;  // 5 or 6, from the header
    bool three = false;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (fields == 0) {
            if (line == index_header_1) {
                fields = 5;
            } else if (line == index_header_2) {
                fields = 6;
            } else if (line == index_header_3) {
                fields = 6;
                three = true;
            } else {
                error = "not a package index (its first line: " + line.substr(0, 40) + ")";
                return std::nullopt;
            }
            continue;
        }
        if (line.empty()) continue;
        std::vector<std::string> f;
        for (std::size_t from = 0; from <= line.size();) {
            std::size_t space = line.find(' ', from);
            if (space == std::string::npos) space = line.size();
            f.push_back(line.substr(from, space - from));
            from = space + 1;
        }
        // Index 3: a seventh field "tar" marks a tarball in pkg's own index.
        const bool tar = three && f.size() == 7 && f[6] == "tar";
        if (tar) f.pop_back();
        bool good = f.size() == fields && good_name(f[0]) && !f[1].empty() && f[1][0] >= '0' &&
                    f[1][0] <= '9' && good_name(f[1]) && hex_of(f[2], 16) && (f[3] == "yes" || f[3] == "no") &&
                    hex_of(f[4], 64);
        Available a{f.size() > 0 ? f[0] : "", f.size() > 1 ? f[1] : "", f.size() > 2 ? f[2] : "",
                    f.size() > 4 ? f[4] : "", f.size() > 3 && f[3] == "yes", {}, three && !tar};
        if (good && fields == 6 && f[5] != "-") {
            for (std::size_t from = 0; from <= f[5].size();) {
                std::size_t comma = f[5].find(',', from);
                if (comma == std::string::npos) comma = f[5].size();
                const std::string need = f[5].substr(from, comma - from);
                good = good && good_name(need) && need != a.name;
                a.needs.push_back(need);
                from = comma + 1;
            }
        }
        if (!good) {
            error = "a bad line in the package index: " + line.substr(0, 80);
            return std::nullopt;
        }
        out.push_back(std::move(a));
    }
    if (fields == 0) {
        error = "an empty package index";
        return std::nullopt;
    }
    return out;
}

std::string format_index(const std::vector<Available>& index) {
    std::string text = std::string(index_header_3) + "\n";
    for (const Available& a : index) {
        std::string needs;
        for (const std::string& n : a.needs) needs += (needs.empty() ? "" : ",") + n;
        text += a.name + " " + a.version + " " + a.key + " " + (a.bundled ? "yes" : "no") + " " + a.sha256 + " " +
                (needs.empty() ? "-" : needs) + (a.cartridge ? "" : " tar") + "\n";
    }
    return text;
}

std::optional<std::string> read_text(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string text;
    std::array<char, 65536> buffer{};
    for (;;) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text.append(buffer.data(), static_cast<std::size_t>(n));
        if (text.size() > (std::size_t{1} << 20)) break;  // an index is a few KB
    }
    ::close(fd);
    return text;
}

bool write_text(const std::string& path, const std::string& text) {
    const std::string part = path + ".part";
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    if (::close(fd) != 0 || !written || ::rename(part.c_str(), path.c_str()) != 0) {
        ::unlink(part.c_str());
        return false;
    }
    return true;
}

std::optional<std::vector<Available>> read_index(const std::string& path, std::string& error) {
    const auto text = read_text(path);
    if (!text) {
        error = errno_text("cannot read", path);
        return std::nullopt;
    }
    auto index = parse_index(*text, error);
    if (!index) error = path + ": " + error;
    return index;
}

/// The index `pkg update` last settled on; nothing if it never has.
std::optional<std::vector<Available>> local_index() {
    std::string error;
    if (::access(index_path().c_str(), F_OK) != 0) return std::nullopt;
    auto index = read_index(index_path(), error);
    if (!index) std::fprintf(stderr, "pkg: %s\n", error.c_str());
    return index;
}

const Available* find_available(const std::vector<Available>& index, std::string_view name) {
    for (const Available& a : index) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

/// `first`, then what `second` has that `first` has not.
std::vector<Available> merged(std::vector<Available> first, const std::vector<Available>& second) {
    std::set<std::string> have;
    for (const Available& a : first) have.insert(a.name);
    for (const Available& a : second) {
        if (!have.contains(a.name)) first.push_back(a);
    }
    return first;
}

/// SHA-256 (FIPS 180-4) of a file, in hex; empty if it cannot be read.
std::string sha256_file(const std::string& path) {
    static constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    const auto block = [&](const unsigned char* p) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = std::uint32_t{p[4 * i]} << 24 | std::uint32_t{p[4 * i + 1]} << 16 |
                   std::uint32_t{p[4 * i + 2]} << 8 | std::uint32_t{p[4 * i + 3]};
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    };
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::array<unsigned char, 65536> buffer{};
    std::array<unsigned char, 64> partial{};
    std::size_t held = 0;
    std::uint64_t total = 0;
    for (;;) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            ::close(fd);
            return {};
        }
        if (n == 0) break;
        total += static_cast<std::uint64_t>(n);
        std::size_t at = 0;
        const std::size_t got = static_cast<std::size_t>(n);
        if (held > 0) {
            const std::size_t take = std::min(got, partial.size() - held);
            std::memcpy(partial.data() + held, buffer.data(), take);
            held += take, at = take;
            if (held < partial.size()) continue;
            block(partial.data());
            held = 0;
        }
        for (; at + 64 <= got; at += 64) block(buffer.data() + at);
        std::memcpy(partial.data(), buffer.data() + at, got - at);
        held = got - at;
    }
    ::close(fd);
    partial[held++] = 0x80;
    if (held > 56) {
        std::fill(partial.begin() + static_cast<std::ptrdiff_t>(held), partial.end(), 0);
        block(partial.data());
        held = 0;
    }
    std::fill(partial.begin() + static_cast<std::ptrdiff_t>(held), partial.begin() + 56, 0);
    const std::uint64_t bits = total * 8;
    for (int i = 0; i < 8; ++i) partial[56 + i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    block(partial.data());
    std::string hex;
    for (const std::uint32_t v : h) {
        std::array<char, 9> part{};
        std::snprintf(part.data(), part.size(), "%08x", v);
        hex += part.data();
    }
    return hex;
}

/// curl, from the prefix: url into `out`. A bar while it downloads when
/// `progress` and the terminal is there to see it. Its exit status, or 127
/// when there is no curl to run.
int curl_to(const std::string& url, const std::string& out, bool progress) {
    std::vector<std::string> args{"curl", "-fL", "--retry", "3", "--connect-timeout", "20"};
    if (progress && ::isatty(STDERR_FILENO)) {
        args.emplace_back("--progress-bar");
    } else {
        args.emplace_back("-s");  // pkg says what went wrong itself
    }
    args.insert(args.end(), {"-o", out, url});
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (::posix_spawnp(&pid, "curl", nullptr, nullptr, argv.data(), environ) != 0) return 127;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    if (!WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}

std::string curl_failed(int status, const std::string& url) {
    if (status == 127) return "no curl to download with (it comes with sqterm: pkg install a curl package file)";
    if (status == 22) return "not in the release: " + url;
    if (status == 6 || status == 7 || status == 28) {
        return "cannot reach " + url.substr(0, url.find('/', 8)) + " (curl " + std::to_string(status) +
               "): offline?";
    }
    return "curl " + std::to_string(status) + " for " + url;
}

/// The index, freshly settled (`pkg update`, and what upgrade and install
/// NAME start with): the release's when it can be reached, with what only
/// the packages folder's index names after it; offline, the packages
/// folder's, then the last one settled on. Kept for `pkg list`. Says where
/// it came from when that is not the release, unless `quiet`.
std::optional<std::vector<Available>> refresh_index(std::string& error, bool quiet = false) {
    const std::string dir = prefix + "/var/lib/sqterm";
    if (!make_directories(dir)) {
        error = errno_text("cannot make", dir);
        return std::nullopt;
    }
    if (from == Source::archive) {
        // What the archive's packages were installed by: the index last
        // settled on.
        auto last = local_index();
        if (!last) error = "no index settled on yet (pkg update, once, from the repo or the packages folder)";
        return last;
    }
    // The release's.
    std::optional<std::vector<Available>> release;
    std::string offline = "--from sqterm";
    if (from != Source::sqterm) {
        // index-3 (the cartridges) first; a release from before it has only
        // index (the tarballs).
        const std::string part = index_path() + ".download";
        for (const char* name : {"index-3", "index"}) {
            const std::string url = release_base() + "/" + name;
            if (const int status = curl_to(url, part, false); status == 0) {
                release = read_index(part, offline);
            } else {
                offline = curl_failed(status, url);
            }
            ::unlink(part.c_str());
            if (release) break;
        }
        if (from == Source::repo && !release) {
            error = offline;
            return std::nullopt;
        }
    }
    // The packages folder's (tools/fetch_packages.sh --get writes it).
    std::optional<std::vector<Available>> folder;
    const std::string folder_index = packages_dir() + "/index";
    if (from == Source::sqterm && ::access(folder_index.c_str(), F_OK) != 0) {
        error = "no " + folder_index + " (in Termux: tools/fetch_packages.sh --get NAME...)";
        return std::nullopt;
    }
    if (from != Source::repo && ::access(folder_index.c_str(), F_OK) == 0) {
        std::string why;
        folder = read_index(folder_index, why);
        if (!folder && !quiet) std::fprintf(stderr, "pkg: %s (left out)\n", why.c_str());
    }
    std::vector<Available> index;
    if (release) {
        index = folder ? merged(*release, *folder) : *release;
    } else if (auto last = from == Source::any ? local_index() : std::nullopt; last && folder) {
        // Offline with both: the newer of the two first (the release's, as
        // the last update settled it, or what Termux fetched since).
        struct stat a{}, b{};
        const bool folder_newer = ::stat(folder_index.c_str(), &a) == 0 && ::stat(index_path().c_str(), &b) == 0 &&
                                  a.st_mtime > b.st_mtime;
        if (!quiet) {
            std::fprintf(stderr, "pkg: %s; using %s\n", offline.c_str(),
                         folder_newer ? folder_index.c_str() : "the index from the last pkg update");
        }
        return folder_newer ? merged(*folder, *last) : merged(*last, *folder);
    } else if (folder) {
        if (!quiet && from == Source::any) std::fprintf(stderr, "pkg: %s; using %s\n", offline.c_str(), folder_index.c_str());
        index = *folder;
    } else if (last) {
        if (!quiet) std::fprintf(stderr, "pkg: %s; using the index from the last pkg update\n", offline.c_str());
        return last;
    } else {
        error = offline + ", and there is no " + folder_index;
        return std::nullopt;
    }
    // Kept for list and info only when it is the whole picture (no --from)
    // or the release's: an index settled from the packages folder alone
    // would make the next plain upgrade go back to what that folder has.
    if ((from == Source::any || from == Source::repo) && !write_text(index_path(), format_index(index))) {
        std::fprintf(stderr, "pkg: %s\n", errno_text("cannot keep", index_path()).c_str());
    }
    return index;
}

/// What is installed, by name: each package's record.
std::map<std::string, Manifest> installed_all() {
    std::map<std::string, Manifest> out;
    DIR* dir = ::opendir(records.c_str());
    if (dir == nullptr) return out;  // nothing installed yet
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(dir)) {
        if (good_name(entry->d_name)) names.emplace_back(entry->d_name);
    }
    ::closedir(dir);
    for (const std::string& name : names) {
        std::string error;
        if (auto m = installed(name, error)) {
            out.emplace(name, std::move(*m));
        } else {
            std::fprintf(stderr, "pkg: %s\n", error.c_str());
        }
    }
    return out;
}

int cmd_list() {
    const auto all = installed_all();
    const auto index = local_index();
    // Columns past the longest name and version (zsh-syntax-highlighting).
    std::size_t name_width = 0, version_width = 0;
    for (const auto& [name, m] : all) {
        name_width = std::max(name_width, m.id.size());
        version_width = std::max(version_width, m.version.size());
    }
    std::size_t behind = 0;
    for (const auto& [name, m] : all) {
        std::string note;
        if (const Available* a = index ? find_available(*index, name) : nullptr; a && a->key != m.key) {
            note = "  -> " + (a->version != m.version ? a->version + " " : std::string()) + a->key;
            ++behind;
        }
        std::printf("%-*s  %-*s  %s%s\n", static_cast<int>(name_width), m.id.c_str(),
                    static_cast<int>(version_width), m.version.c_str(),
                    m.key.empty() ? "-" : m.key.c_str(), note.c_str());
    }
    if (behind > 0) std::printf("pkg: %zu differ from the index (pkg upgrade)\n", behind);
    return 0;
}

int cmd_files(std::string_view name) {
    std::string error;
    const auto m = installed(name, error);
    if (!m) return fail(error);
    for (const std::string& file : m->files) std::printf("%s\n", file.c_str());
    return 0;
}

/// NAME-<version>[-key].sq, .tar.zst or .tar: the version starts with a
/// digit, so zsh never matches zsh-autosuggestions-0.7.1.tar.zst.
bool archive_of(std::string_view file, std::string_view name) {
    if (!file.starts_with(name) || file.size() <= name.size() + 1 || file[name.size()] != '-') return false;
    const char next = file[name.size() + 1];
    if (next < '0' || next > '9') return false;
    return file.ends_with(".sq") || file.ends_with(".tar.zst") || file.ends_with(".tar");
}

/// The newest NAME archive in a folder, by time; empty if none.
std::string newest_in(const std::string& dir, const std::string& name) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return {};
    std::string best;
    struct timespec best_time{};
    while (const dirent* e = ::readdir(d)) {
        if (!archive_of(e->d_name, name)) continue;
        const std::string path = dir + "/" + e->d_name;
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (best.empty() || st.st_mtim.tv_sec > best_time.tv_sec ||
            (st.st_mtim.tv_sec == best_time.tv_sec && st.st_mtim.tv_nsec > best_time.tv_nsec)) {
            best = path;
            best_time = st.st_mtim;
        }
    }
    ::closedir(d);
    return best;
}

int cmd_install(const std::string& file, bool consume, std::string* id = nullptr) {
    if (!make_directories(records)) return fail(errno_text("cannot make", records));
    const std::string incoming = records + "/.incoming";
    remove_tree(incoming);
    std::array<char, 512> error{};
    // A cartridge (.sq) through sqcart's reader; a tarball through unpack.cpp.
    const auto unpack = file.ends_with(".sq") ? sqterm::unpack_cartridge : sqterm::unpack_archive;
    if (unpack(file.c_str(), prefix.c_str(), incoming.c_str(), error.data(), error.size()) != 0) {
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
    if (id != nullptr) *id = fresh->id;
    if (consume) {
        if (::unlink(file.c_str()) != 0) return fail(errno_text("installed, but cannot delete", file));
        std::printf("pkg: %s deleted\n", file.c_str());
    }
    return 0;
}

/// Where an indexed package's archive is: the internal archive, the
/// packages folder or Download when one holds that very archive (checked
/// against the index's sha256), else downloaded from the release into the
/// internal archive. `from_user` is set when it is a file of the user's (in
/// shared storage), which --consume may delete; empty, with the reason in
/// error, when there is none.
std::string obtain(const Available& a, bool& from_user, std::string& error) {
    from_user = false;
    std::vector<std::string> dirs;
    if (from == Source::any || from == Source::archive) dirs.push_back(archive_dir());
    if (from == Source::any || from == Source::sqterm) dirs.push_back(packages_dir());
    if (from == Source::any) dirs.emplace_back(download_dir);
    for (const std::string& dir : dirs) {
        const std::string path = dir + "/" + a.asset();
        if (::access(path.c_str(), R_OK) != 0) continue;
        if (sha256_file(path) == a.sha256) {
            from_user = dir != archive_dir();
            return path;
        }
        std::fprintf(stderr, "pkg: %s does not match the index's sha256 (left alone)\n", path.c_str());
    }
    if (from == Source::archive || from == Source::sqterm) {
        error = "no " + a.asset() + " in " + (from == Source::archive ? archive_dir() : packages_dir()) +
                " (--from " + std::string(source_name(from)) + ")";
        return {};
    }
    if (!make_directories(archive_dir())) {
        error = errno_text("cannot make", archive_dir());
        return {};
    }
    const std::string file = archive_dir() + "/" + a.asset();
    const std::string part = file + ".part";
    const std::string url = release_base() + "/" + a.asset();
    std::printf("pkg: downloading %s\n", a.asset().c_str());
    std::fflush(stdout);
    if (const int status = curl_to(url, part, true); status != 0) {
        ::unlink(part.c_str());
        error = curl_failed(status, url) + "\n     or put " + a.asset() + " in " + packages_dir() +
                " (in Termux: tools/fetch_packages.sh --get " + a.name + ")";
        return {};
    }
    if (const std::string sum = sha256_file(part); sum != a.sha256) {
        ::unlink(part.c_str());
        error = a.asset() + ": sha256 " + (sum.empty() ? "unreadable" : sum) + ", the index says " + a.sha256 +
                " (a broken download: try again)";
        return {};
    }
    if (::rename(part.c_str(), file.c_str()) != 0) {
        ::unlink(part.c_str());
        error = errno_text("cannot keep", file);
        return {};
    }
    return file;
}

/// One indexed package, installed from wherever obtain finds it. The
/// internal archive keeps one archive a package: this one.
int install_available(const Available& a, bool consume) {
    bool from_user = false;
    std::string error;
    const std::string file = obtain(a, from_user, error);
    if (file.empty()) return fail(error);
    std::printf("pkg: %s\n", file.c_str());
    const int result = cmd_install(file, consume && from_user);
    if (result == 0 && !from_user) {
        DIR* d = ::opendir(archive_dir().c_str());
        while (const dirent* e = d != nullptr ? ::readdir(d) : nullptr) {
            if (archive_of(e->d_name, a.name) && e->d_name != a.asset()) {
                static_cast<void>(::unlink((archive_dir() + "/" + e->d_name).c_str()));
            }
        }
        if (d != nullptr) ::closedir(d);
    }
    return result;
}

/// What `name` needs at run time and is not installed, installed first,
/// and what that needs before it. `installed` grows as they are.
int install_needs(const std::vector<Available>& index, const std::string& name, std::set<std::string>& installed,
                  std::vector<std::string>& chain) {
    const Available* a = find_available(index, name);
    if (a == nullptr) return 0;
    if (std::find(chain.begin(), chain.end(), name) != chain.end()) {
        return fail("the index's needs go round in a circle at " + name);
    }
    chain.push_back(name);
    for (const std::string& need : a->needs) {
        if (installed.contains(need)) continue;
        const Available* n = find_available(index, need);
        if (n == nullptr) {
            chain.pop_back();
            return fail(name + " needs " + need + ", which no index names");
        }
        if (install_needs(index, need, installed, chain) != 0) return 1;
        std::printf("pkg: %s needs %s\n", name.c_str(), need.c_str());
        if (install_available(*n, false) != 0) return 1;
        installed.insert(need);
    }
    chain.pop_back();
    return 0;
}

std::set<std::string> installed_names() {
    std::set<std::string> out;
    for (const auto& [name, m] : installed_all()) out.insert(name);
    return out;
}

/// After an install from a file: what its index entry needs, if pkg knows
/// it (the last index settled on; no network for that).
void needs_after_file(const std::string& name) {
    const auto index = local_index();
    if (!index) return;
    auto have = installed_names();
    std::vector<std::string> chain;
    static_cast<void>(install_needs(*index, name, have, chain));
}

int cmd_install_file(const std::string& file, bool consume) {
    std::string name;
    const int result = cmd_install(file, consume, &name);
    if (result == 0) needs_after_file(name);
    return result;
}

int cmd_remove(std::string_view name, bool force) {
    std::string error;
    const auto m = installed(name, error);
    if (!m) return fail(error);
    if (!force) {
        if (const auto index = local_index()) {
            const auto have = installed_names();
            std::string needed_by;
            for (const Available& a : *index) {
                if (have.contains(a.name) && std::find(a.needs.begin(), a.needs.end(), name) != a.needs.end()) {
                    needed_by += (needed_by.empty() ? "" : ", ") + a.name;
                }
            }
            if (!needed_by.empty()) {
                return fail(std::string(name) + " is needed by " + needed_by + " (pkg remove " + std::string(name) +
                            " --force to remove it anyway)");
            }
        }
    }
    delete_files(m->files);
    if (!remove_tree(record_of(name))) return fail(errno_text("cannot remove", record_of(name)));
    std::printf("pkg: %s %s removed\n", m->id.c_str(), m->version.c_str());
    return 0;
}

int cmd_update() {
    std::string error;
    const auto index = refresh_index(error);
    if (!index) return fail(error);
    const auto all = installed_all();
    std::vector<std::string> behind;
    for (const auto& [name, m] : all) {
        if (const Available* a = find_available(*index, name); a && a->key != m.key) behind.push_back(name);
    }
    std::printf("pkg: %zu packages known; %zu installed differ", index->size(), behind.size());
    for (std::size_t i = 0; i < behind.size(); ++i) std::printf("%s%s", i == 0 ? ": " : ", ", behind[i].c_str());
    std::printf("%s\n", behind.empty() ? "" : " (pkg upgrade)");
    return 0;
}

int cmd_upgrade(const std::vector<std::string>& names) {
    std::string error;
    const auto index = refresh_index(error);
    if (!index) return fail(error);
    const auto all = installed_all();
    for (const std::string& name : names) {
        if (!all.contains(name)) return fail(name + " is not installed (pkg install " + name + ")");
        if (!find_available(*index, name)) return fail("no " + name + " in the index");
    }
    // In the index's order: what a package depends on before it.
    std::vector<const Available*> todo;
    for (const Available& a : *index) {
        const auto it = all.find(a.name);
        if (it == all.end() || it->second.key == a.key) continue;
        if (!names.empty() && std::find(names.begin(), names.end(), a.name) == names.end()) continue;
        todo.push_back(&a);
    }
    auto have = installed_names();
    int failed = 0;
    for (const Available* a : todo) {
        std::vector<std::string> chain;
        if (install_needs(*index, a->name, have, chain) != 0 || install_available(*a, false) != 0) ++failed;
    }
    // What is installed and needs what is not (lldb without runtimes).
    std::size_t added = 0;
    for (const Available& a : *index) {
        if (!have.contains(a.name)) continue;
        if (!names.empty() && std::find(names.begin(), names.end(), a.name) == names.end()) continue;
        const std::size_t before = have.size();
        std::vector<std::string> chain;
        if (install_needs(*index, a.name, have, chain) != 0) ++failed;
        added += have.size() - before;
    }
    if (failed > 0) return fail(std::to_string(failed) + " not upgraded");
    if (todo.empty() && added == 0) {
        std::printf("pkg: %s up to date\n", names.empty() ? "everything is" : "already");
    }
    return 0;
}

int cmd_search(std::string_view word) {
    std::string error;
    const auto index = refresh_index(error);
    if (!index) return fail(error);
    const auto all = installed_all();
    std::size_t width = 0;
    for (const Available& a : *index) width = std::max(width, a.name.size());
    for (const Available& a : *index) {
        if (a.name.find(word) == std::string::npos) continue;
        const auto it = all.find(a.name);
        std::string notes;
        if (!a.bundled) notes += "  (not in the APK)";
        if (!a.needs.empty()) {
            notes += "  needs";
            for (const std::string& n : a.needs) notes += " " + n;
        }
        if (it != all.end()) notes += it->second.key == a.key ? "  installed" : "  installed, differs";
        std::printf("%-*s  %s%s\n", static_cast<int>(width), a.name.c_str(), a.version.c_str(), notes.c_str());
    }
    return 0;
}

std::string megabytes(std::uint64_t bytes) {
    std::array<char, 32> text{};
    if (bytes < (std::uint64_t{1} << 20)) {
        std::snprintf(text.data(), text.size(), "%llu KB", static_cast<unsigned long long>((bytes + 1023) >> 10));
    } else {
        std::snprintf(text.data(), text.size(), "%.1f MB", static_cast<double>(bytes) / (1 << 20));
    }
    return text.data();
}

/// `pkg info NAME`: what is installed of it, what the index says, and where
/// its archive already is. No network: the index last settled on.
int cmd_info(const std::string& name) {
    if (!good_name(name)) return fail("a bad package name: " + name);
    std::string error;
    const auto m = installed(name, error);
    const auto index = local_index();
    const Available* a = index ? find_available(*index, name) : nullptr;
    if (!m && a == nullptr) {
        return fail(name + " is not installed, and no index names it (pkg update, pkg search)");
    }
    const auto row = [](const char* label, const std::string& value) { std::printf("%-12s %s\n", label, value.c_str()); };
    row("name", name);
    if (m) {
        std::uint64_t bytes = 0;
        for (const std::string& f : m->files) {
            struct stat st{};
            if (::lstat((prefix + "/" + f).c_str(), &st) == 0) bytes += static_cast<std::uint64_t>(st.st_size);
        }
        row("installed", m->version + " (" + (m->key.empty() ? "no key" : m->key) + "), " +
                             std::to_string(m->files.size()) + " files, " + megabytes(bytes));
        if (!m->license.empty()) row("license", m->license);
    } else {
        row("installed", "no");
    }
    if (a != nullptr) {
        row("index", a->version + " (" + a->key + ")" + (m && m->key == a->key ? ", the same" : m ? ", differs (pkg upgrade)" : ""));
        row("in the APK", a->bundled ? "yes" : "no");
        std::string needs;
        for (const std::string& n : a->needs) needs += (needs.empty() ? "" : " ") + n;
        row("needs", needs.empty() ? "nothing" : needs);
        // Where this very archive already is (named, not checked: install checks).
        std::string here;
        const std::pair<const char*, std::string> places[] = {
            {"archive", archive_dir()}, {"sqterm", packages_dir()}, {"download", std::string(download_dir)}};
        for (const auto& [label, dir] : places) {
            if (::access((dir + "/" + a->asset()).c_str(), R_OK) == 0) here += (here.empty() ? "" : ", ") + std::string(label);
        }
        row("archive at", here.empty() ? "the repo only" : here + ", and the repo");
    }
    std::string needed_by;
    if (index) {
        const auto have = installed_names();
        for (const Available& b : *index) {
            if (have.contains(b.name) && std::find(b.needs.begin(), b.needs.end(), name) != b.needs.end()) {
                needed_by += (needed_by.empty() ? "" : " ") + b.name;
            }
        }
    }
    if (!needed_by.empty()) row("needed by", needed_by);
    return 0;
}

/// `pkg owns PATH`: which package installed it. PATH absolute (in the
/// prefix), relative to the prefix, or a bare command name (bin/NAME).
int cmd_owns(const std::string& what) {
    std::vector<std::string> wanted;
    if (what.starts_with(prefix + "/")) {
        wanted.push_back(what.substr(prefix.size() + 1));
    } else if (what.starts_with("/")) {
        // Through a link into the prefix (/data/user/0 is /data/data).
        std::array<char, PATH_MAX> real{}, real_prefix{};
        if (::realpath(what.c_str(), real.data()) != nullptr && ::realpath(prefix.c_str(), real_prefix.data()) != nullptr &&
            std::string_view(real.data()).starts_with(std::string(real_prefix.data()) + "/")) {
            wanted.emplace_back(real.data() + std::strlen(real_prefix.data()) + 1);
        } else {
            return fail(what + " is not in the prefix (" + prefix + ")");
        }
    } else {
        wanted.push_back(what);
        if (what.find('/') == std::string::npos) wanted.push_back("bin/" + what);
    }
    // And what a link there points to, inside the prefix (coreutils' links).
    for (std::size_t i = 0, n = wanted.size(); i < n; ++i) {
        std::array<char, PATH_MAX> real{}, real_prefix{};
        if (::realpath((prefix + "/" + wanted[i]).c_str(), real.data()) != nullptr &&
            ::realpath(prefix.c_str(), real_prefix.data()) != nullptr) {
            const std::string r = real.data(), rp = std::string(real_prefix.data()) + "/";
            if (r.starts_with(rp) && r.substr(rp.size()) != wanted[i]) wanted.push_back(r.substr(rp.size()));
        }
    }
    bool found = false;
    for (const auto& [name, m] : installed_all()) {
        for (const std::string& f : m.files) {
            if (std::find(wanted.begin(), wanted.end(), f) != wanted.end()) {
                std::printf("%s: %s\n", name.c_str(), f.c_str());
                found = true;
            }
        }
    }
    if (!found) return fail("no installed package has " + what);
    return 0;
}

/// `pkg install NAME`: the indexed package, and what it needs first, from
/// wherever obtain finds them; a name no index knows (or no index at all):
/// the newest archive of it in the packages folder, then Download.
int install_by_name(const std::string& name, bool consume) {
    std::string error;
    const auto index = refresh_index(error, true);
    if (index) {
        if (const Available* a = find_available(*index, name)) {
            auto have = installed_names();
            std::vector<std::string> chain;
            if (install_needs(*index, name, have, chain) != 0) return 1;
            return install_available(*a, consume);
        }
    }
    std::vector<std::string> dirs;
    if (from == Source::archive) dirs.push_back(archive_dir());
    if (from == Source::any || from == Source::sqterm) dirs.push_back(packages_dir());
    if (from == Source::any) dirs.emplace_back(download_dir);
    for (const std::string& dir : dirs) {
        const std::string found = newest_in(dir, name);
        if (!found.empty()) {
            std::printf("pkg: %s (no index names %s: not checked)\n", found.c_str(), name.c_str());
            return cmd_install_file(found, consume);
        }
    }
    if (!index) return fail(error);
    if (from != Source::any) return fail("no " + name + " --from " + std::string(source_name(from)) + " (pkg search --from " + std::string(source_name(from)) + ")");
    return fail("no " + name + " in the index, " + packages_dir() + " or " + std::string(download_dir) +
                " (pkg search)");
}

int cmd_clean() {
    DIR* d = ::opendir(archive_dir().c_str());
    if (d == nullptr) {
        std::printf("pkg: the archive is empty\n");
        return 0;
    }
    std::uint64_t bytes = 0;
    std::size_t count = 0;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string path = archive_dir() + "/" + e->d_name;
        struct stat st{};
        if (::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::unlink(path.c_str()) == 0) {
            bytes += static_cast<std::uint64_t>(st.st_size);
            ++count;
        }
    }
    ::closedir(d);
    std::printf("pkg: %zu archive(s) deleted, %llu MB freed\n", count,
                static_cast<unsigned long long>((bytes + (1 << 19)) >> 20));
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

// The package's version (packages/pkg/recipe.sh gives it).
#ifndef SQTERM_PKG_VERSION
#define SQTERM_PKG_VERSION "unknown"
#endif

/// The commands, on `out`: stderr after a mistake (status 2), stdout when
/// asked for with --help (status 0).
int usage(std::FILE* out = stderr, int status = 2) {
    std::fprintf(out,
                 "usage: pkg list\n"
                 "       pkg info NAME\n"
                 "       pkg files NAME\n"
                 "       pkg owns PATH|COMMAND\n"
                 "       pkg update [--from SOURCE]\n"
                 "       pkg upgrade [NAME...] [--from SOURCE]\n"
                 "       pkg search [WORD] [--from SOURCE]\n"
                 "       pkg install NAME|FILE... [--consume] [--from SOURCE]\n"
                 "       pkg remove NAME [--force]\n"
                 "       pkg backup NAME [DIR]\n"
                 "       pkg clean\n"
                 "       pkg --help | --version\n"
                 "SOURCE: archive (pkg's own), sqterm (/sdcard/sqterm/packages), repo (the release)\n");
    return status;
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
    static constexpr std::string_view commands[] = {"list", "info", "files", "owns", "update", "upgrade", "search",
                                                    "install", "remove", "backup", "clean"};
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
    if (argc > 1) {
        const std::string_view first = argv[1];
        if (first == "--help" || first == "-h" || first == "help") return usage(stdout, 0);
        if (first == "--version" || first == "version") {
            std::printf("pkg %s\n", SQTERM_PKG_VERSION);
            return 0;
        }
    }
    const char* p = std::getenv("PREFIX");
    if (p == nullptr || *p != '/') return fail("PREFIX is not set (sqterm's shell sets it)");
    prefix = p;
    records = prefix + "/var/lib/sqterm/pkg";
    // Options anywhere after the command: --consume, --force, --from SOURCE.
    std::vector<std::string> args;
    bool consume = false, force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string_view source;
        if (arg == "--consume") {
            consume = true;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--from" || arg.starts_with("--from=")) {
            if (arg == "--from") {
                if (i + 1 >= argc) return fail("--from needs a source: archive, sqterm or repo");
                source = argv[++i];
            } else {
                source = arg.substr(7);
            }
            if (source == "archive") {
                from = Source::archive;
            } else if (source == "sqterm") {
                from = Source::sqterm;
            } else if (source == "repo") {
                from = Source::repo;
            } else {
                return fail("--from " + std::string(source) + ": the sources are archive, sqterm and repo");
            }
        } else {
            args.emplace_back(arg);
        }
    }
    if (args.empty()) return usage();
    const std::string& cmd = args[0];
    const bool sourced = cmd == "install" || cmd == "upgrade" || cmd == "update" || cmd == "search";
    if (from != Source::any && !sourced) return fail("--from is for install, upgrade, update and search");
    if (consume && cmd != "install") return fail("--consume is for install");
    if (force && cmd != "remove") return fail("--force is for remove");
    if (cmd == "list" && args.size() == 1) return cmd_list();
    if (cmd == "info" && args.size() == 2) return cmd_info(args[1]);
    if (cmd == "files" && args.size() == 2) return cmd_files(args[1]);
    if (cmd == "owns" && args.size() == 2) return cmd_owns(args[1]);
    if (cmd == "update" && args.size() == 1) return cmd_update();
    if (cmd == "upgrade") return cmd_upgrade({args.begin() + 1, args.end()});
    if (cmd == "search" && args.size() <= 2) return cmd_search(args.size() == 2 ? args[1] : std::string());
    if (cmd == "install" && args.size() >= 2) {
        // Each in turn (pkg install llvm sqedit apkpack): one that fails is
        // said and the rest still go in; the status says whether any failed.
        int failed = 0;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& what = args[i];
            // A path, or a file here: as given. A bare package name: the
            // index's (install_by_name).
            struct stat st{};
            int status = 0;
            if (what.find('/') == std::string::npos && ::stat(what.c_str(), &st) != 0 && good_name(what)) {
                status = install_by_name(what, consume);
            } else if (from != Source::any) {
                status = fail("--from is for installing by name, not a file (" + what + ")");
            } else {
                status = cmd_install_file(what, consume);
            }
            if (status != 0) ++failed;
        }
        if (failed > 0 && args.size() > 2) return fail(std::to_string(failed) + " of " + std::to_string(args.size() - 1) + " not installed");
        return failed > 0 ? 1 : 0;
    }
    if (cmd == "remove" && args.size() == 2) return cmd_remove(args[1], force);
    if (cmd == "clean" && args.size() == 1) return cmd_clean();
    if (cmd == "backup" && (args.size() == 2 || args.size() == 3)) {
        return cmd_backup(args[1], args.size() == 3 ? args[2] : std::string("."));
    }
    return unknown(cmd);
}
