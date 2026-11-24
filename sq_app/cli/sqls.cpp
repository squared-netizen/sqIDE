// sqls: ls with icons and colours, for sqterm (sqterm-tools). Each name gets
// a Nerd Font icon by its kind (a folder, a program, a link) or its name and
// extension (C++, Lua, JSON, an archive, a picture), coloured as LS_COLORS
// says, which sqterm's zshrc sets to follow its theme. sqterm bundles the
// icons' font (Nerd Fonts' Symbols Only).
//
//   sqls [-a] [-A] [-l] [-1] [-d] [-r] [-t] [-S] [--tree [-L N]] [--no-icons]
//        [--color=auto|always|never] [PATH...]
//
//   -a        every name, . and .. too       -A   every name but . and ..
//   -l        long: kind and permissions, size, when changed, the link's target
//   -1        one name a line                -d   a folder itself, not its contents
//   -r        reversed                       -t   newest first   -S   largest first
//   --tree    the folders under it as a tree (-L N: N levels deep)
//   --no-icons, or SQLS_ICONS=0: names only, coloured
//
// Folders come first, then names case-blind. With no terminal (a pipe), one
// name a line and no colour, as ls does, unless --color=always.
//
// This file is YOURS (ownership class: seeded, sqterm's own).

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    bool all = false;         // -a
    bool almost_all = false;  // -A
    bool long_form = false;   // -l
    bool one = false;         // -1
    bool directory = false;   // -d
    bool reverse = false;     // -r
    bool by_time = false;     // -t
    bool by_size = false;     // -S
    bool tree = false;        // --tree
    int levels = 1 << 20;     // -L
    bool icons = true;
    bool color = true;
};

struct Entry {
    std::string name;  // as shown
    std::string path;  // to stat and read
    struct stat st{};
    bool stat_ok = false;
    bool is_link = false;
    std::string target;   // a link's
    bool target_ok = true;
    struct stat target_st{};
};

// --- colours: LS_COLORS ------------------------------------------------------

struct Colours {
    std::map<std::string, std::string, std::less<>> kinds;       // di, ln, ex, fi, ...
    std::map<std::string, std::string, std::less<>> extensions;  // *.cpp -> "94"; *README -> "37"

    [[nodiscard]] const std::string* kind(std::string_view key) const {
        const auto it = kinds.find(key);
        return it == kinds.end() ? nullptr : &it->second;
    }
};

Colours read_colours() {
    Colours c;
    // What GNU ls does with none set, near enough: folders blue, links cyan, programs green.
    c.kinds = {{"di", "01;34"}, {"ln", "01;36"}, {"ex", "01;32"}, {"pi", "33"}, {"so", "01;35"},
               {"bd", "01;33"}, {"cd", "01;33"}, {"or", "01;31"}};
    const char* env = getenv("LS_COLORS");
    if (env == nullptr) return c;
    std::string_view rest(env);
    while (!rest.empty()) {
        const std::size_t colon = rest.find(':');
        const std::string_view item = rest.substr(0, colon);
        rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key(item.substr(0, eq)), value(item.substr(eq + 1));
        if (key.size() > 1 && key[0] == '*') {
            c.extensions[key.substr(1)] = value;
        } else {
            c.kinds[key] = value;
        }
    }
    return c;
}

[[nodiscard]] std::string lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

[[nodiscard]] bool ends_with(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

/// The SGR for an entry: its kind's, else the longest pattern its name ends with.
[[nodiscard]] std::string colour_of(const Colours& c, const Entry& e) {
    const auto kind = [&](std::string_view k) -> std::string {
        const std::string* v = c.kind(k);
        return v != nullptr ? *v : std::string();
    };
    if (e.is_link) return e.target_ok ? kind("ln") : kind("or");
    if (!e.stat_ok) return kind("or");
    const mode_t m = e.st.st_mode;
    if (S_ISDIR(m)) return kind("di");
    if (S_ISFIFO(m)) return kind("pi");
    if (S_ISSOCK(m)) return kind("so");
    if (S_ISBLK(m)) return kind("bd");
    if (S_ISCHR(m)) return kind("cd");
    if ((m & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) return kind("ex");
    std::size_t best = 0;
    std::string found;
    for (const auto& [pattern, value] : c.extensions) {
        if (pattern.size() > best && ends_with(e.name, pattern)) {
            best = pattern.size();
            found = value;
        }
    }
    return found.empty() ? kind("fi") : found;
}

// --- icons: Nerd Fonts v3 ----------------------------------------------------

/// A name's icon, as UTF-8: by kind, then by whole name, then by extension.
[[nodiscard]] std::string_view icon_of(const Entry& e) {
    static const std::map<std::string, std::string_view, std::less<>> names{
        {".git", "\uf1d3"},           {".gitignore", "\uf1d3"},  {".gitmodules", "\uf1d3"},
        {"build.lua", "\ue620"},      {"makefile", "\ue615"},    {"license", "\uf0fc"},
        {"readme.md", "\uf48a"},      {"readme", "\uf48a"},      {".zshrc", "\uf489"},
        {".zprofile", "\uf489"},      {".bashrc", "\uf489"},     {"agents.md", "\uf48a"},
        {"sq-inf", "\uf487"},         {"node_modules", "\ue718"}, {"cmakelists.txt", "\ue615"},
    };
    static const std::map<std::string, std::string_view, std::less<>> extensions{
        {"c", "\ue61e"},     {"h", "\uf0fd"},     {"cpp", "\ue61d"},  {"cc", "\ue61d"},   {"cxx", "\ue61d"},
        {"cppm", "\ue61d"},  {"ixx", "\ue61d"},   {"hpp", "\uf0fd"},  {"lua", "\ue620"},  {"py", "\ue606"},
        {"sh", "\uf489"},    {"zsh", "\uf489"},   {"bash", "\uf489"}, {"fish", "\uf489"}, {"rs", "\ue7a8"},
        {"go", "\ue627"},    {"js", "\ue74e"},    {"ts", "\ue628"},   {"java", "\ue738"}, {"kt", "\ue634"},
        {"json", "\ue60b"},  {"toml", "\ue615"},  {"yml", "\ue615"},  {"yaml", "\ue615"}, {"ini", "\ue615"},
        {"conf", "\ue615"},  {"xml", "\ue619"},   {"html", "\ue60e"}, {"css", "\ue614"},  {"md", "\uf48a"},
        {"txt", "\uf15c"},   {"rst", "\uf15c"},   {"pdf", "\uf1c1"},  {"log", "\uf18d"},  {"peg", "\ue615"},
        {"zip", "\uf410"},   {"tar", "\uf410"},   {"gz", "\uf410"},   {"tgz", "\uf410"},  {"xz", "\uf410"},
        {"zst", "\uf410"},   {"bz2", "\uf410"},   {"7z", "\uf410"},   {"deb", "\uf410"},  {"sq", "\uf487"},
        {"apk", "\ue70e"},   {"dex", "\ue70e"},   {"png", "\uf1c5"},  {"jpg", "\uf1c5"},  {"jpeg", "\uf1c5"},
        {"gif", "\uf1c5"},   {"webp", "\uf1c5"},  {"svg", "\uf1c5"},  {"ico", "\uf1c5"},  {"ttf", "\uf031"},
        {"otf", "\uf031"},   {"fnt", "\uf031"},   {"mp3", "\uf001"},  {"ogg", "\uf001"},  {"wav", "\uf001"},
        {"flac", "\uf001"},  {"mp4", "\uf03d"},   {"mkv", "\uf03d"},  {"webm", "\uf03d"}, {"o", "\uf471"},
        {"a", "\uf471"},     {"so", "\uf471"},    {"pcm", "\uf471"},  {"lock", "\uf023"}, {"pem", "\uf084"},
        {"key", "\uf084"},   {"diff", "\uf440"},  {"patch", "\uf440"},
    };
    if (e.is_link) return e.target_ok && e.target_st.st_mode != 0 && S_ISDIR(e.target_st.st_mode) ? "\uf482" : "\uf481";
    const std::string name = lower(e.name);
    if (const auto it = names.find(name); it != names.end()) return it->second;
    if (e.stat_ok && S_ISDIR(e.st.st_mode)) return name == "." || name == ".." ? "\uf07c" : "\uf07b";
    if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot + 1 < name.size()) {
        if (const auto it = extensions.find(std::string_view(name).substr(dot + 1)); it != extensions.end()) {
            return it->second;
        }
    }
    if (e.stat_ok && (e.st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) return "\uf489";
    return "\uf15b";
}

// --- reading ---------------------------------------------------------------

Entry entry_of(std::string name, std::string path) {
    Entry e;
    e.name = std::move(name);
    e.path = std::move(path);
    e.stat_ok = lstat(e.path.c_str(), &e.st) == 0;
    if (e.stat_ok && S_ISLNK(e.st.st_mode)) {
        e.is_link = true;
        std::string target(4096, '\0');
        const ssize_t n = readlink(e.path.c_str(), target.data(), target.size());
        target.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
        e.target = std::move(target);
        e.target_ok = stat(e.path.c_str(), &e.target_st) == 0;
    }
    return e;
}

[[nodiscard]] bool is_dir(const Entry& e) {
    return (e.stat_ok && S_ISDIR(e.st.st_mode)) || (e.is_link && e.target_ok && S_ISDIR(e.target_st.st_mode));
}

std::vector<Entry> read_folder(const std::string& folder, const Options& o) {
    std::vector<Entry> out;
    DIR* d = opendir(folder.c_str());
    if (d == nullptr) {
        fprintf(stderr, "sqls: %s: %s\n", folder.c_str(), strerror(errno));
        return out;
    }
    while (const dirent* de = readdir(d)) {
        const std::string name = de->d_name;
        const bool hidden = !name.empty() && name[0] == '.';
        const bool dots = name == "." || name == "..";
        if (hidden && !o.all && !(o.almost_all && !dots)) continue;
        out.push_back(entry_of(name, folder == "/" ? "/" + name : folder + "/" + name));
    }
    closedir(d);
    return out;
}

void sort_entries(std::vector<Entry>& v, const Options& o) {
    std::sort(v.begin(), v.end(), [&](const Entry& a, const Entry& b) {
        if (o.by_time && a.st.st_mtime != b.st.st_mtime) return a.st.st_mtime > b.st.st_mtime;
        if (o.by_size && a.st.st_size != b.st.st_size) return a.st.st_size > b.st.st_size;
        const bool ad = is_dir(a), bd = is_dir(b);
        if (ad != bd) return ad;
        const std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    if (o.reverse) std::reverse(v.begin(), v.end());
}

// --- writing ---------------------------------------------------------------

/// Columns a UTF-8 string takes: one a character (the icons are one-cell Mono glyphs).
[[nodiscard]] std::size_t width_of(std::string_view s) {
    std::size_t n = 0;
    for (const char ch : s) {
        if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) ++n;
    }
    return n;
}

struct Writer {
    const Options& o;
    const Colours& colours;

    /// An entry's icon and name, coloured; and how many columns it takes.
    [[nodiscard]] std::string label(const Entry& e, std::size_t* columns = nullptr) const {
        std::string out;
        std::size_t w = 0;
        const std::string sgr = o.color ? colour_of(colours, e) : std::string();
        if (!sgr.empty()) out += "\x1b[" + sgr + "m";
        if (o.icons) {
            out += icon_of(e);
            out += ' ';
            w += 2;
        }
        out += e.name;
        w += width_of(e.name);
        if (!sgr.empty()) out += "\x1b[0m";
        if (columns != nullptr) *columns = w;
        return out;
    }

    void grid(const std::vector<Entry>& v) const {
        if (v.empty()) return;
        std::size_t screen = 80;
        if (winsize ws{}; isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            screen = ws.ws_col;
        }
        std::vector<std::string> labels;
        std::vector<std::size_t> widths;
        for (const Entry& e : v) {
            std::size_t w = 0;
            labels.push_back(label(e, &w));
            widths.push_back(w);
        }
        // The most columns that fit, filled down then across, as ls does.
        std::size_t columns = 1;
        std::vector<std::size_t> column_widths{*std::max_element(widths.begin(), widths.end())};
        for (std::size_t c = v.size(); c > 1; --c) {
            const std::size_t rows = (v.size() + c - 1) / c;
            std::vector<std::size_t> cw((v.size() + rows - 1) / rows, 0);
            for (std::size_t i = 0; i < v.size(); ++i) cw[i / rows] = std::max(cw[i / rows], widths[i]);
            std::size_t total = 0;
            for (std::size_t w : cw) total += w + 2;
            if (total - 2 <= screen) {
                columns = cw.size();
                column_widths = cw;
                break;
            }
        }
        const std::size_t rows = (v.size() + columns - 1) / columns;
        for (std::size_t r = 0; r < rows; ++r) {
            std::string line;
            for (std::size_t c = 0; c < columns; ++c) {
                const std::size_t i = c * rows + r;
                if (i >= v.size()) break;
                line += labels[i];
                const bool last = c + 1 == columns || (c + 1) * rows + r >= v.size();
                if (!last) line.append(column_widths[c] - widths[i] + 2, ' ');
            }
            printf("%s\n", line.c_str());
        }
    }

    void lines(const std::vector<Entry>& v) const {
        for (const Entry& e : v) printf("%s\n", label(e).c_str());
    }

    [[nodiscard]] static std::string mode_of(const Entry& e) {
        const mode_t m = e.st.st_mode;
        std::string s = S_ISDIR(m) ? "d" : S_ISLNK(m) ? "l" : S_ISFIFO(m) ? "p" : S_ISSOCK(m) ? "s" : S_ISCHR(m) ? "c" : S_ISBLK(m) ? "b" : "-";
        const char* rwx = "rwxrwxrwx";
        for (int i = 0; i < 9; ++i) s += (m & (1U << (8 - i))) != 0 ? rwx[i] : '-';
        return s;
    }

    [[nodiscard]] static std::string size_of(const Entry& e) {
        if (S_ISDIR(e.st.st_mode)) return "-";
        double size = static_cast<double>(e.st.st_size);
        const char* units = "BKMGT";
        int u = 0;
        while (size >= 1024.0 && u < 4) {
            size /= 1024.0;
            ++u;
        }
        char text[32];
        if (u == 0) {
            snprintf(text, sizeof text, "%lld", static_cast<long long>(e.st.st_size));
        } else {
            snprintf(text, sizeof text, size < 10.0 ? "%.1f%c" : "%.0f%c", size, units[u]);
        }
        return text;
    }

    [[nodiscard]] static std::string when_of(const Entry& e) {
        const time_t t = e.st.st_mtime;
        tm local{};
        localtime_r(&t, &local);
        char text[32];
        const time_t now = time(nullptr);
        // Within half a year: the day and time; else the day and year, as ls.
        strftime(text, sizeof text, std::abs(static_cast<double>(now - t)) < 182.0 * 24 * 3600 ? "%b %e %H:%M" : "%b %e  %Y",
                 &local);
        return text;
    }

    void long_lines(const std::vector<Entry>& v) const {
        std::size_t size_width = 1;
        for (const Entry& e : v) size_width = std::max(size_width, size_of(e).size());
        const std::string dim = o.color ? "\x1b[90m" : "", reset = o.color ? "\x1b[0m" : "";
        for (const Entry& e : v) {
            std::string line = dim + mode_of(e) + reset + "  ";
            const std::string size = size_of(e);
            line += std::string(size_width - size.size(), ' ') + size + "  ";
            line += dim + when_of(e) + reset + "  " + label(e);
            if (e.is_link) line += dim + " -> " + reset + e.target;
            printf("%s\n", line.c_str());
        }
    }

    void list(const std::vector<Entry>& v) const {
        if (o.long_form) {
            long_lines(v);
        } else if (o.one) {
            lines(v);
        } else {
            grid(v);
        }
    }

    void tree(const std::string& folder, const std::string& prefix, int depth) const {
        if (depth >= o.levels) return;
        std::vector<Entry> v = read_folder(folder, o);
        std::erase_if(v, [](const Entry& e) { return e.name == "." || e.name == ".."; });
        sort_entries(v, o);
        const std::string dim = o.color ? "\x1b[90m" : "", reset = o.color ? "\x1b[0m" : "";
        for (std::size_t i = 0; i < v.size(); ++i) {
            const bool last = i + 1 == v.size();
            printf("%s%s%s%s%s\n", dim.c_str(), prefix.c_str(), last ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 ",
                   reset.c_str(), label(v[i]).c_str());
            if (is_dir(v[i]) && !v[i].is_link) {
                tree(v[i].path, prefix + (last ? "    " : "\u2502   "), depth + 1);
            }
        }
    }
};

int usage() {
    fprintf(stderr,
            "usage: sqls [-a] [-A] [-l] [-1] [-d] [-r] [-t] [-S] [--tree [-L N]] [--no-icons]\n"
            "            [--color=auto|always|never] [PATH...]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string color = "auto";
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--") {
            for (++i; i < argc; ++i) paths.emplace_back(argv[i]);
            break;
        }
        if (a == "--tree") {
            o.tree = true;
        } else if (a == "--no-icons") {
            o.icons = false;
        } else if (a.starts_with("--color")) {
            color = a.size() > 8 ? std::string(a.substr(8)) : "always";
        } else if (a == "-L" && i + 1 < argc) {
            o.levels = std::max(1, atoi(argv[++i]));
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
            for (const char f : a.substr(1)) {
                switch (f) {
                    case 'a': o.all = true; break;
                    case 'A': o.almost_all = true; break;
                    case 'l': o.long_form = true; break;
                    case '1': o.one = true; break;
                    case 'd': o.directory = true; break;
                    case 'r': o.reverse = true; break;
                    case 't': o.by_time = true; break;
                    case 'S': o.by_size = true; break;
                    case 'h': break;  // sizes are always human
                    default: return usage();
                }
            }
        } else if (a.starts_with("--")) {
            return usage();
        } else {
            paths.emplace_back(a);
        }
    }
    const bool tty = isatty(STDOUT_FILENO) != 0;
    o.color = color == "always" || (color == "auto" && tty);
    if (!tty && !o.long_form && !o.tree) o.one = true;
    if (const char* icons = getenv("SQLS_ICONS"); icons != nullptr && strcmp(icons, "0") == 0) o.icons = false;
    if (paths.empty()) paths.emplace_back(".");
    const Colours colours = read_colours();
    const Writer w{o, colours};

    int status = 0;
    std::vector<Entry> files;
    std::vector<std::string> folders;
    for (const std::string& p : paths) {
        Entry e = entry_of(p, p);
        if (!e.stat_ok) {
            fprintf(stderr, "sqls: %s: %s\n", p.c_str(), strerror(errno));
            status = 2;
        } else if (is_dir(e) && !o.directory) {
            folders.push_back(p);
        } else {
            files.push_back(std::move(e));
        }
    }
    sort_entries(files, o);
    w.list(files);
    for (std::size_t i = 0; i < folders.size(); ++i) {
        if (paths.size() > 1) printf("%s%s:\n", files.empty() && i == 0 ? "" : "\n", folders[i].c_str());
        if (o.tree) {
            printf("%s\n", w.label(entry_of(folders[i], folders[i])).c_str());
            w.tree(folders[i], "", 0);
            continue;
        }
        std::vector<Entry> v = read_folder(folders[i], o);
        sort_entries(v, o);
        w.list(v);
    }
    return status;
}
