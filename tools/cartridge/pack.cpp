// SPDX-License-Identifier: MIT
//
// sqterm-pack INSTALL OUT.sq: a built package as a squared cartridge, 3.0
// (sqcart's spec, Appendix B.2: kind "package"). INSTALL is the package's
// install (build/packages/<name>/dest$PREFIX), with the SQ-INF/manifest.json
// packages/build.sh wrote: its consumers.sqterm.files are what goes in.
//
//   a regular file   an entry: flags r, and x when any execute bit is set;
//                    read at commit, chunked past 64 MiB, never held whole
//
// Many small files (headers, terminfo, zsh's functions) compress far less
// one by one than in a tarball's single stream, so when a package has at
// least 8 files of at most 1 MiB, a zstd dictionary is trained on them
// (§4.3: SQ-INF/dictionaries/<id>.zdict) and they are compressed with it.
//   a symbolic link  no entry: consumers.sqterm.links names it and its target,
//                    for pkg to make (a cartridge holds no link, §6.1)
//
// The manifest goes in as format_version 3, in canonical form (RFC 8785),
// with consumers.sqterm.links added, and "case_sensitive": true when two of
// its paths collide under case folding (§4.1: the sysroot's Linux headers,
// xt_MARK.h and xt_mark.h; a package unpacks to $PREFIX, which does not
// fold case). The commit's time is 0, so the same
// install gives the same bytes. Written beside OUT, then renamed over it.

#include <sys/stat.h>
#include <unistd.h>

import std;
import sq.data;
import sq.cart;
import sq.cart.write;

namespace {

int fail(std::string_view what) {
    std::println(std::cerr, "sqterm-pack: {}", what);
    return 1;
}

std::optional<std::string> slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>(in), {}};
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    namespace flag = sq::cart::flag;
    if (argc != 3) {
        std::println(std::cerr, "usage: sqterm-pack INSTALL OUT.sq");
        return 2;
    }
    const fs::path install = argv[1], out = argv[2];
    const auto text = slurp(install / "SQ-INF" / "manifest.json");
    if (!text) return fail(std::format("no SQ-INF/manifest.json in {}", install.string()));
    auto parsed = sq::data::parse_json(*text, {.ijson = true});
    if (!parsed) return fail("SQ-INF/manifest.json: " + parsed.error.message);
    sq::data::JsonValue manifest = std::move(parsed.value);
    auto* sqterm = manifest.find("consumers") ? manifest.find("consumers")->find("sqterm") : nullptr;
    const auto* files = sqterm ? sqterm->find("files") : nullptr;
    if (!sqterm || !sqterm->object_if() || !files || !files->array_if()) return fail("the manifest has no consumers.sqterm.files");

    fs::path temporary = out;
    temporary += ".new";
    std::error_code ec;
    fs::remove(temporary, ec);
    sq::cart::WriteOptions options;
    options.commit_time = 0;
    auto w = sq::cart::Writer::create(temporary, options);
    if (!w) return fail(w.error().message);

    // Small files, for a dictionary.
    constexpr std::uint64_t small = 1024 * 1024, sample_budget = std::uint64_t{32} << 20;
    std::vector<std::vector<std::byte>> samples;
    std::uint64_t sampled = 0;
    for (const auto& item : *files->array_if()) {
        const std::string* path = item.string_if();
        struct stat st{};
        if (!path || ::lstat((install / *path).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        const auto n = static_cast<std::uint64_t>(st.st_size);
        if (n == 0 || n > small || sampled + n > sample_budget) continue;
        std::ifstream in(install / *path, std::ios::binary);
        std::vector<std::byte> b(static_cast<std::size_t>(n));
        if (!in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n))) continue;
        sampled += n;
        samples.push_back(std::move(b));
    }
    std::uint32_t dictionary = 0;
    if (samples.size() >= 8) {
        std::vector<std::span<const std::byte>> spans(samples.begin(), samples.end());
        auto id = w->train_dictionary(spans, 112 * 1024);
        if (id) dictionary = *id;  // else none: each file compressed alone
    }
    samples.clear();

    sq::data::JsonValue::Object links;
    std::uint64_t entries = 0;
    std::map<std::u32string, std::string> folded;
    std::optional<std::pair<std::string, std::string>> collision;
    for (const auto& item : *files->array_if()) {
        const std::string* path = item.string_if();
        if (!path) return fail("consumers.sqterm.files holds something that is not a path");
        const fs::path full = install / *path;
        struct stat st{};
        if (::lstat(full.c_str(), &st) != 0) return fail(std::format("{}: listed, but not in the install", *path));
        if (S_ISLNK(st.st_mode)) {
            const fs::path target = fs::read_symlink(full, ec);
            if (ec) return fail(std::format("{}: cannot read the link: {}", *path, ec.message()));
            links.insert_or_assign(*path, sq::data::JsonValue(target.string()));
            continue;
        }
        if (!S_ISREG(st.st_mode)) return fail(std::format("{}: neither a file nor a symbolic link", *path));
        if (auto [it, fresh] = folded.try_emplace(sq::cart::collision_key(*path), *path); !fresh && !collision) collision = {it->second, *path};
        const std::uint16_t flags = flag::r | ((st.st_mode & 0111) ? flag::x : 0);
        sq::cart::PutOptions put{.flags = flags};
        if (dictionary != 0 && static_cast<std::uint64_t>(st.st_size) <= small) put.dictionary = dictionary;
        if (auto r = w->put_file(*path, full, put); !r) return fail(std::format("{}: {}", *path, r.error().message));
        ++entries;
    }

    manifest.object_if()->insert_or_assign("format_version", sq::data::JsonValue(std::uint64_t{3}));
    if (collision) {
        manifest.object_if()->insert_or_assign("case_sensitive", sq::data::JsonValue(true));
        std::println("{}: case_sensitive ({} and {} collide under case folding)", out.string(), collision->first, collision->second);
    }
    sqterm->object_if()->insert_or_assign("links", sq::data::JsonValue(std::move(links)));
    auto written = sq::data::write_json(manifest, {.canonical = true});
    if (!written) return fail("the manifest: " + written.error.message);
    const auto bytes = std::as_bytes(std::span(written.text));
    if (auto r = w->put("SQ-INF/manifest.json", bytes, {.media_type = "application/json"}); !r) return fail(r.error().message);
    auto c = w->commit();
    if (!c) return fail(c.error().message);
    w = std::unexpected(sq::cart::Error{});  // the lock goes before the rename
    fs::rename(temporary, out, ec);
    if (ec) return fail(std::format("cannot rename {} to {}: {}", temporary.string(), out.string(), ec.message()));
    std::error_code ignore;
    fs::path lock = temporary;
    lock += ".lock";
    fs::remove(lock, ignore);
    std::println("{}: {} entries, {} links, {} bytes", out.string(), entries,
                 sqterm->find("links")->object_if()->size(), fs::file_size(out));
    return 0;
}
