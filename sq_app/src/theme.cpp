// sqterm: its colours. A theme is the 16 named colours programs choose from
// (SGR 30-37 and 90-97: what ls, zsh and vim mean by "red" or "bright
// blue"), the text and background where a program chooses none, the cursor
// and the selection.
//
// This file is YOURS (ownership class: seeded, sqterm's own).
//
// The default is "cyberpunk dream": a deep violet-black, with neon magenta,
// cyan, mint and acid yellow, and lavender text. Any of it can be changed
// without a rebuild, in ~/.config/sqterm/theme.json, read when sqterm starts
// and again when term.theme asks (each end reads its own copy: the Shell the
// palette, the widget the rest, so no thread reads what another writes):
//
//   {
//     "background": "#0b0719",
//     "foreground": "#e6e1ff",
//     "cursor":     "#ff2a6d",
//     "selection":  "#5b2a86",
//     "palette": ["#1b1035", "#ff3864", ... 16 colours, or fewer: the rest stay]
//   }
//
// Every key is optional; a colour is "#rrggbb". A file that does not parse,
// or a colour that is not one, is said on stderr (the app's log) and the
// default kept for it.

#include <stdio.h>

#include "sqterm.hpp"

namespace sqterm {

namespace {

constexpr sq::term::Rgb hex(std::uint32_t v) noexcept {
    return {static_cast<std::uint8_t>((v >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((v >> 8U) & 0xFFU), static_cast<std::uint8_t>(v & 0xFFU)};
}

/// "cyberpunk dream": neon on a deep violet-black.
constexpr Theme cyberpunk_dream{
    .name = "cyberpunk dream",
    .foreground = hex(0xE6E1FF),  // lavender white
    .background = hex(0x0B0719),  // violet-black
    .cursor = hex(0xFF2A6D),      // neon magenta
    .selection = hex(0x5B2A86),   // deep purple
    .palette = {{
        hex(0x1B1035),  // black: a lifted violet, so it shows on the background
        hex(0xFF3864),  // red: hot pink-red
        hex(0x2DE2A6),  // green: neon mint
        hex(0xFFD319),  // yellow: acid yellow
        hex(0x4D8DFF),  // blue: electric blue
        hex(0xD45CFF),  // magenta: violet neon
        hex(0x05D9E8),  // cyan: neon cyan
        hex(0xC8C3E8),  // white: lavender grey
        hex(0x5A4A82),  // bright black: muted violet (comments, hints)
        hex(0xFF6B8B),  // bright red
        hex(0x72F1B8),  // bright green
        hex(0xFFF07A),  // bright yellow
        hex(0x7AA2FF),  // bright blue
        hex(0xFF71CE),  // bright magenta: hot pink
        hex(0x7AF8FF),  // bright cyan
        hex(0xF5F3FF),  // bright white
    }},
};

/// "#rrggbb" as a colour; nothing for anything else.
[[nodiscard]] std::optional<sq::term::Rgb> colour_of(const sq::data::JsonValue* value) {
    const std::string* s = value != nullptr ? value->string_if() : nullptr;
    if (s == nullptr || s->size() != 7 || (*s)[0] != '#') return std::nullopt;
    std::uint32_t v = 0;
    const auto [end, error] = std::from_chars(s->data() + 1, s->data() + 7, v, 16);
    if (error != std::errc() || end != s->data() + 7) return std::nullopt;
    return hex(v);
}

}  // namespace

Theme read_theme(std::string_view home) {
    Theme t = cyberpunk_dream;
    const std::string path = std::string(home) + "/.config/sqterm/theme.json";
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return t;  // none: the default
    std::string text;
    std::array<char, 4096> buffer{};
    while (const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), file))
        text.append(buffer.data(), n);
    std::fclose(file);
    const auto parsed = sq::data::parse_json(text);
    if (!parsed || parsed.value.object_if() == nullptr) {
        std::fprintf(stderr, "sqterm: %s is not a JSON object; the default theme stays\n",
                     path.c_str());
        return t;
    }
    const auto set = [&](std::string_view key, sq::term::Rgb& into) {
        const sq::data::JsonValue* value = parsed.value.find(key);
        if (value == nullptr) return;
        if (const auto c = colour_of(value)) {
            into = *c;
        } else {
            std::fprintf(stderr, "sqterm: %s: %.*s is not \"#rrggbb\"\n", path.c_str(),
                         static_cast<int>(key.size()), key.data());
        }
    };
    set("foreground", t.foreground);
    set("background", t.background);
    set("cursor", t.cursor);
    set("selection", t.selection);
    if (const sq::data::JsonValue* palette = parsed.value.find("palette")) {
        if (const auto* list = palette->array_if()) {
            for (std::size_t i = 0; i < list->size() && i < t.palette.size(); ++i) {
                if (const auto c = colour_of(&(*list)[i])) {
                    t.palette[i] = *c;
                } else {
                    std::fprintf(stderr, "sqterm: %s: palette[%zu] is not \"#rrggbb\"\n",
                                 path.c_str(), i);
                }
            }
        }
    }
    t.name = "theme.json";
    return t;
}

}  // namespace sqterm
