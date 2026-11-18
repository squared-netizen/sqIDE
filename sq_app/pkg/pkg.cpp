// pkg: sqterm's packages, in the shell (12.5).
//
// This file is YOURS (ownership class: seeded, sqterm's own).
//
//   pkg list                         what is installed: name, version, key,
//                                    and the release's key where it differs
//   pkg files NAME                   the files a package installed
//   pkg update                       the release's index: what it has
//   pkg upgrade [NAME...]            each installed package (or those named)
//                                    whose key differs from the release's,
//                                    downloaded and installed
//   pkg search [WORD]                the release's packages (with WORD in the
//                                    name), and which are installed
//   pkg install NAME                 from the release, as built for this
//                                    key; offline, the newest
//                                    NAME-<version>-*.tar.zst (or .tar) in
//                                    the download folder ($PKG_DIR, default
//                                    /sdcard/Download)
//   pkg install FILE [--consume]     a package from a file (.tar.zst, or the
//                                    .tar pkg backup writes); --consume
//                                    deletes FILE once it is installed
//   pkg remove NAME                  its files, then its record
//   pkg backup NAME [DIR]            its installed files as a package again,
//                                    DIR/NAME-VERSION.tar (default: here)
//
// The network, only when asked: update, upgrade and install NAME download
// from the GitHub release named `packages` (where CI publishes every
// package, .github/workflows/packages.yml), through curl (a package of its
// own, in the APK). The release's `index` names, for each package, the
// version, key and sha256 of the archive built from main's recipes; pkg
// keeps the last one it fetched in $PREFIX/var/lib/sqterm/release-index and
// checks every archive against its sha256 before installing it. That sha256
// guards against a broken download, as HTTPS guards the way there; neither
// is a signature (sqcart 3.0 defines those). SQ_PACKAGES_URL (default
// https://github.com/squared-netizen/sqterm/releases/download) points
// elsewhere: a mirror, or a test. Everything else reads files only.
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

// ---- the release: its index, sha256, curl ----

/// One line of the release's index: an archive CI built from main's recipes.
struct Available {
    std::string name;
    std::string version;
    std::string key;
    std::string sha256;
    bool bundled = true;  ///< in the APK as well

    std::string asset() const { return name + "-" + version + "-" + key + ".tar.zst"; }
};

constexpr std::string_view index_header = "sqterm release index 1";

std::string index_path() { return prefix + "/var/lib/sqterm/release-index"; }
std::string cache_dir() { return prefix + "/var/cache/sqterm/pkg"; }

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

/// The release index as text: its header, then "name version key bundled
/// sha256" a line, in the order the packages build (what one depends on
/// before it). Empty, with the reason in error, when it is not one.
std::optional<std::vector<Available>> parse_index(const std::string& text, std::string& error) {
    std::vector<Available> out;
    std::size_t at = 0;
    bool first = true;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (first) {
            if (line != index_header) {
                error = "not a release index (its first line: " + line.substr(0, 40) + ")";
                return std::nullopt;
            }
            first = false;
            continue;
        }
        if (line.empty()) continue;
        std::array<std::string, 5> f;
        std::size_t field = 0, from = 0;
        for (; field < f.size() && from <= line.size(); ++field) {
            std::size_t space = line.find(' ', from);
            if (space == std::string::npos) space = line.size();
            f[field] = line.substr(from, space - from);
            from = space + 1;
        }
        if (field != f.size() || from <= line.size() || !good_name(f[0]) || f[1].empty() ||
            !(f[1][0] >= '0' && f[1][0] <= '9') || !good_name(f[1]) || !hex_of(f[2], 16) ||
            (f[3] != "yes" && f[3] != "no") || !hex_of(f[4], 64)) {
            error = "a bad line in the release index: " + line.substr(0, 80);
            return std::nullopt;
        }
        out.push_back({f[0], f[1], f[2], f[4], f[3] == "yes"});
    }
    if (first) {
        error = "an empty release index";
        return std::nullopt;
    }
    return out;
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

/// The index `pkg update` last fetched; nothing if it never has.
std::optional<std::vector<Available>> local_index() {
    const auto text = read_text(index_path());
    if (!text) return std::nullopt;
    std::string error;
    auto index = parse_index(*text, error);
    if (!index) std::fprintf(stderr, "pkg: %s: %s\n", index_path().c_str(), error.c_str());
    return index;
}

const Available* find_available(const std::vector<Available>& index, std::string_view name) {
    for (const Available& a : index) {
        if (a.name == name) return &a;
    }
    return nullptr;
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
        args.emplace_back("-sS");
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

/// `pkg update`: the release's index, fetched and checked, then kept.
/// Quiet: say nothing but errors.
std::optional<std::vector<Available>> fetch_index(std::string& error) {
    const std::string dir = prefix + "/var/lib/sqterm";
    if (!make_directories(dir)) {
        error = errno_text("cannot make", dir);
        return std::nullopt;
    }
    const std::string part = index_path() + ".part";
    const std::string url = release_base() + "/index";
    const int status = curl_to(url, part, false);
    if (status != 0) {
        ::unlink(part.c_str());
        error = curl_failed(status, url);
        return std::nullopt;
    }
    const auto text = read_text(part);
    auto index = text ? parse_index(*text, error) : std::nullopt;
    if (!text) error = errno_text("cannot read", part);
    if (!index || ::rename(part.c_str(), index_path().c_str()) != 0) {
        if (index) error = errno_text("cannot keep", index_path());
        ::unlink(part.c_str());
        return std::nullopt;
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
    if (behind > 0) std::printf("pkg: %zu differ from the release (pkg upgrade)\n", behind);
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

/// One package from the release: downloaded into the cache, checked against
/// the index's sha256, installed, and the download deleted.
int install_available(const Available& a) {
    if (!make_directories(cache_dir())) return fail(errno_text("cannot make", cache_dir()));
    const std::string file = cache_dir() + "/" + a.asset();
    const std::string part = file + ".part";
    const std::string url = release_base() + "/" + a.asset();
    std::printf("pkg: %s %s (%s), from the release\n", a.name.c_str(), a.version.c_str(), a.key.c_str());
    std::fflush(stdout);
    const int status = curl_to(url, part, true);
    if (status != 0) {
        ::unlink(part.c_str());
        return fail(curl_failed(status, url));
    }
    const std::string sum = sha256_file(part);
    if (sum != a.sha256) {
        ::unlink(part.c_str());
        return fail(a.asset() + ": sha256 " + (sum.empty() ? "unreadable" : sum) + ", the index says " +
                    a.sha256 + " (a broken download: try again)");
    }
    if (::rename(part.c_str(), file.c_str()) != 0) {
        ::unlink(part.c_str());
        return fail(errno_text("cannot keep", file));
    }
    const int result = cmd_install(file, false);
    ::unlink(file.c_str());
    return result;
}

int cmd_update() {
    std::string error;
    const auto index = fetch_index(error);
    if (!index) return fail(error);
    const auto all = installed_all();
    std::vector<std::string> behind;
    for (const auto& [name, m] : all) {
        if (const Available* a = find_available(*index, name); a && a->key != m.key) behind.push_back(name);
    }
    std::printf("pkg: the release has %zu packages; %zu installed differ", index->size(), behind.size());
    for (std::size_t i = 0; i < behind.size(); ++i) std::printf("%s%s", i == 0 ? ": " : ", ", behind[i].c_str());
    std::printf("%s\n", behind.empty() ? "" : " (pkg upgrade)");
    return 0;
}

int cmd_upgrade(const std::vector<std::string>& names) {
    std::string error;
    const auto index = fetch_index(error);
    if (!index) return fail(error);
    const auto all = installed_all();
    for (const std::string& name : names) {
        if (!all.contains(name)) return fail(name + " is not installed (pkg install " + name + ")");
        if (!find_available(*index, name)) return fail("no " + name + " in the release");
    }
    // In the index's order: what a package depends on before it.
    std::vector<const Available*> todo;
    for (const Available& a : *index) {
        const auto it = all.find(a.name);
        if (it == all.end() || it->second.key == a.key) continue;
        if (!names.empty() && std::find(names.begin(), names.end(), a.name) == names.end()) continue;
        todo.push_back(&a);
    }
    if (todo.empty()) {
        std::printf("pkg: %s up to date with the release\n", names.empty() ? "everything is" : "already");
        return 0;
    }
    int failed = 0;
    for (const Available* a : todo) {
        if (install_available(*a) != 0) ++failed;
    }
    if (failed > 0) return fail(std::to_string(failed) + " of " + std::to_string(todo.size()) + " not upgraded");
    return 0;
}

int cmd_search(std::string_view word) {
    std::string error;
    auto index = fetch_index(error);
    if (!index) {
        index = local_index();
        if (!index) return fail(error);
        std::fprintf(stderr, "pkg: %s; the index from the last pkg update:\n", error.c_str());
    }
    const auto all = installed_all();
    std::size_t width = 0;
    for (const Available& a : *index) width = std::max(width, a.name.size());
    for (const Available& a : *index) {
        if (a.name.find(word) == std::string::npos) continue;
        const auto it = all.find(a.name);
        const char* state = it == all.end() ? "" : it->second.key == a.key ? "  installed" : "  installed, differs";
        std::printf("%-*s  %s%s%s\n", static_cast<int>(width), a.name.c_str(), a.version.c_str(),
                    a.bundled ? "" : "  (large: not in the APK)", state);
    }
    return 0;
}

/// `pkg install NAME`: the release's, as built for its key; the one in the
/// download folder when it is that very archive (nothing to download), or,
/// when the release cannot be reached, the newest one there.
int install_by_name(const std::string& name, bool consume) {
    std::string error;
    const auto index = fetch_index(error);
    if (index) {
        const Available* a = find_available(*index, name);
        if (a == nullptr) return fail("no " + name + " in the release (pkg search)");
        const std::string local = download_dir() + "/" + a->asset();
        if (::access(local.c_str(), R_OK) == 0 && sha256_file(local) == a->sha256) {
            std::printf("pkg: %s\n", local.c_str());
            return cmd_install(local, consume);
        }
        return install_available(*a);
    }
    std::fprintf(stderr, "pkg: %s; looking in %s\n", error.c_str(), download_dir().c_str());
    const std::string found = find_archive(name, error);
    if (found.empty()) return fail(error);
    std::printf("pkg: %s\n", found.c_str());
    return cmd_install(found, consume);
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
                 "       pkg update\n"
                 "       pkg upgrade [NAME...]\n"
                 "       pkg search [WORD]\n"
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
    static constexpr std::string_view commands[] = {"list", "files", "update", "upgrade", "search",
                                                    "install", "remove", "backup"};
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
    if (cmd == "update" && args.size() == 1) return cmd_update();
    if (cmd == "upgrade") return cmd_upgrade({args.begin() + 1, args.end()});
    if (cmd == "search" && args.size() <= 2) return cmd_search(args.size() == 2 ? args[1] : std::string());
    if (cmd == "install" && (args.size() == 2 || args.size() == 3)) {
        // --consume before or after the package.
        const bool consume = args.size() == 3;
        if (consume && args[1] != "--consume" && args[2] != "--consume") return usage();
        const std::string& what = args.size() == 3 && args[1] == "--consume" ? args[2] : args[1];
        // A path, or a file here: as given. A bare package name: the
        // release's (install_by_name).
        struct stat st{};
        if (what.find('/') == std::string::npos && ::stat(what.c_str(), &st) != 0 && good_name(what)) {
            return install_by_name(what, consume);
        }
        return cmd_install(what, consume);
    }
    if (cmd == "remove" && args.size() == 2) return cmd_remove(args[1]);
    if (cmd == "backup" && (args.size() == 2 || args.size() == 3)) {
        return cmd_backup(args[1], args.size() == 3 ? args[2] : std::string("."));
    }
    return unknown(cmd);
}
