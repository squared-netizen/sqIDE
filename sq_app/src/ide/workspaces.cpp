// sqIDE: the Workspaces screen (workspaces.hpp says what is here).
//
// This file is sqIDE's own (sqterm has none of it).

#include "workspaces.hpp"

#include <stdio.h>  // popen, pclose

namespace sqide {

namespace {

// The theme's colours (cyberpunk dream), as the terminal draws them.
constexpr sq::graphics::Color background{
    .red = 0.043F, .green = 0.027F, .blue = 0.098F, .alpha = 1.0F};
constexpr sq::graphics::Color card_color{
    .red = 0.106F, .green = 0.063F, .blue = 0.208F, .alpha = 1.0F};
constexpr sq::graphics::Color pressed_color{
    .red = 0.18F, .green = 0.11F, .blue = 0.33F, .alpha = 1.0F};
constexpr sq::graphics::Color text_color{.red = 0.90F, .green = 0.88F, .blue = 1.0F, .alpha = 1.0F};
constexpr sq::graphics::Color dim_color{.red = 0.62F, .green = 0.58F, .blue = 0.78F, .alpha = 1.0F};
constexpr sq::graphics::Color cyan{.red = 0.02F, .green = 0.85F, .blue = 0.91F, .alpha = 1.0F};
constexpr sq::graphics::Color magenta{.red = 1.0F, .green = 0.16F, .blue = 0.43F, .alpha = 1.0F};

constexpr float margin = 16.0F;
constexpr float button_top = 52.0F;
constexpr float button_height = 52.0F;
constexpr float cards_top = button_top + button_height + margin;
constexpr float card_height = 84.0F;
constexpr float card_gap = 10.0F;
/// A finger that moves this far is scrolling, not tapping.
constexpr float drag_slop = 10.0F;

[[nodiscard]] bool inside(const sq::gui::Rectangle& r, float x, float y) noexcept {
    return r.width > 0.0F && x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

/// `s` in single quotes for the shell, each ' as '\''.
[[nodiscard]] std::string shell_quoted(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

[[nodiscard]] std::string text_of(const sq::data::JsonValue* value) {
    const std::string* s = value != nullptr ? value->string_if() : nullptr;
    return s != nullptr ? *s : std::string();
}

}  // namespace

// ---- the model's part ---------------------------------------------------------------------------

Workspaces::Workspaces(sqterm::Places places) : places_(std::move(places)) {}

void Workspaces::start(sq::messaging::Telegraph& t) {
    find(t);
}

void Workspaces::receive(sq::messaging::Telegraph& t, const sq::messaging::Received& received) {
    if (received.telegram.address().name == addresses::refresh.name()) {
        find(t);
    }
}

void Workspaces::find(sq::messaging::Telegraph& t) {
    // square from the prefix, as the shell would run it; its JSON as it is.
    const std::string square = places_.prefix + "/bin/square";
    const std::string command =
        shell_quoted(square) + " --json find " + shell_quoted(places_.home) + " 2>/dev/null";
    std::string out;
    if (FILE* pipe = ::popen(command.c_str(), "r")) {
        std::array<char, 4096> buffer{};
        while (const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
            out.append(buffer.data(), n);
        }
        if (::pclose(pipe) != 0) {
            out.clear();
        }
    }
    if (out.empty()) {
        out = "{\"error\": \"square found nothing (is it installed? pkg install square)\"}";
    }
    const std::span<std::byte> room = t.snapshot(addresses::cards, out.size());
    if (room.size() < out.size()) {
        return;
    }
    std::memcpy(room.data(), out.data(), out.size());
    static_cast<void>(t.publish(addresses::cards, out.size()));
}

// ---- the widget ---------------------------------------------------------------------------------

WorkspacesView::WorkspacesView(sq::Stage& stage) noexcept : stage_(stage) {
    // Shown again (back from the terminal): the cards as they are now.
    static_cast<void>(stage_.telegraph().do_(addresses::refresh));
}

sq::gui::Size WorkspacesView::preferred_size(sq::gui::Painter& /*painter*/,
                                             const sq::gui::Skin& /*skin*/) const {
    return {320.0F, 480.0F};  // it grows to fill the screen
}

void WorkspacesView::act(double delta_seconds) {
    sq::gui::Widget::act(delta_seconds);
    if (const auto shot = stage_.telegraph().snapshot(addresses::cards);
        shot && (!have_cards_ || shot->sequence != sequence_)) {
        sequence_ = shot->sequence;
        have_cards_ = true;
        read_cards({reinterpret_cast<const char*>(shot->bytes.data()), shot->bytes.size()});
    }
}

void WorkspacesView::read_cards(std::string_view json) {
    cards_.clear();
    message_.clear();
    const auto parsed = sq::data::parse_json(json);
    if (!parsed) {
        message_ = "square's answer is not JSON";
        return;
    }
    if (const std::string error = text_of(parsed.value.find("error")); !error.empty()) {
        message_ = error;
        return;
    }
    const sq::data::JsonValue* list = parsed.value.find("workspaces");
    if (list != nullptr && list->array_if() != nullptr) {
        for (const auto& w : *list->array_if()) {
            cards_.push_back({.path = text_of(w.find("path")),
                              .form = text_of(w.find("form")),
                              .id = text_of(w.find("id")),
                              .title = text_of(w.find("title")),
                              .version = text_of(w.find("version")),
                              .template_id = text_of(w.find("template"))});
        }
    }
    if (cards_.empty()) {
        message_ = "No workspaces yet: New makes one (sqpg new).";
    }
}

sq::gui::Rectangle WorkspacesView::button(std::size_t i) const noexcept {
    const float w = (width() - (margin * 4.0F)) / 3.0F;
    return {margin + (static_cast<float>(i) * (w + margin)), button_top, w, button_height};
}

sq::gui::Rectangle WorkspacesView::card(std::size_t i) const noexcept {
    return {margin, cards_top + scroll_ + (static_cast<float>(i) * (card_height + card_gap)),
            width() - (margin * 2.0F), card_height};
}

WorkspacesView::Target WorkspacesView::target_at(float x, float y) const noexcept {
    for (std::size_t i = 0; i < 3; ++i) {
        if (inside(button(i), x, y)) {
            return {.kind = i == 0   ? Target::Kind::terminal
                            : i == 1 ? Target::Kind::make_new
                                     : Target::Kind::refresh};
        }
    }
    if (y < cards_top) {
        return {};
    }
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        if (inside(card(i), x, y)) {
            return {.kind = Target::Kind::card, .card = i};
        }
    }
    return {};
}

void WorkspacesView::paint(sq::gui::Painter& painter, const sq::gui::Skin& /*skin*/, float stage_x,
                           float stage_y) const {
    painter.fill_rectangle({stage_x, stage_y, width(), height()}, background);
    painter.draw_text("sqIDE  ·  Workspaces", stage_x + margin, stage_y + margin, cyan);

    // The cards, scrolled, under the buttons.
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const sq::gui::Rectangle r = card(i);
        if (r.y + r.height < cards_top || r.y > height()) {
            continue;
        }
        const Card& c = cards_[i];
        const bool held = pressed_.kind == Target::Kind::card && pressed_.card == i && !dragging_;
        painter.fill_rectangle({stage_x + r.x, stage_y + r.y, r.width, r.height},
                               held ? pressed_color : card_color);
        // A stripe: cyan for a folder, magenta for a .sq (exploded when opened).
        painter.fill_rectangle({stage_x + r.x, stage_y + r.y, 4.0F, r.height},
                               c.form == "compressed" ? magenta : cyan);
        const float x = stage_x + r.x + 14.0F;
        const float y = stage_y + r.y + 10.0F;
        painter.draw_text(c.title.empty() ? c.id : c.title, x, y, text_color);
        std::string second = c.id + "  " + c.version;
        if (!c.template_id.empty()) {
            second += "  ·  " + c.template_id;
        }
        if (c.form == "compressed") {
            second += "  ·  .sq";
        }
        painter.draw_text(second, x, y + 24.0F, dim_color);
        painter.draw_text(c.path, x, y + 48.0F, dim_color);
    }
    if (!message_.empty()) {
        painter.draw_text(message_, stage_x + margin, stage_y + cards_top + margin, dim_color);
    }

    // The buttons over everything that scrolls under them.
    painter.fill_rectangle({stage_x, stage_y, width(), cards_top - 4.0F}, background);
    painter.draw_text("sqIDE  ·  Workspaces", stage_x + margin, stage_y + margin, cyan);
    static constexpr std::array<std::string_view, 3> labels{"Terminal", "New", "Refresh"};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const sq::gui::Rectangle r = button(i);
        const bool held = !dragging_ && pressed_.kind != Target::Kind::none &&
                          pressed_.kind != Target::Kind::card &&
                          static_cast<std::size_t>(pressed_.kind) - 1 == i;
        painter.fill_rectangle({stage_x + r.x, stage_y + r.y, r.width, r.height},
                               held ? pressed_color : card_color);
        const sq::gui::Size size = painter.measure_text(labels[i]);
        painter.draw_text(labels[i], stage_x + r.x + ((r.width - size.width) * 0.5F),
                          stage_y + r.y + ((r.height - size.height) * 0.5F), text_color);
    }
}

bool WorkspacesView::pointer_event(const sq::gui::PointerEvent& event) {
    using sq::gui::PointerAction;
    switch (event.action) {
        case PointerAction::down:
            pressed_ = target_at(event.x, event.y);
            press_y_ = last_y_ = event.y;
            dragging_ = false;
            return true;
        case PointerAction::move: {
            if (!dragging_ && std::abs(event.y - press_y_) > drag_slop) {
                dragging_ = true;
            }
            if (dragging_) {
                const float content = static_cast<float>(cards_.size()) * (card_height + card_gap);
                const float lowest = std::min(0.0F, height() - cards_top - content - margin);
                scroll_ = std::clamp(scroll_ + (event.y - last_y_), lowest, 0.0F);
            }
            last_y_ = event.y;
            return true;
        }
        case PointerAction::up:
            if (!dragging_ && pressed_.kind != Target::Kind::none &&
                target_at(event.x, event.y) == pressed_) {
                tap(pressed_);
            }
            pressed_ = {};
            dragging_ = false;
            return true;
        case PointerAction::cancel:
            pressed_ = {};
            dragging_ = false;
            return true;
    }
    return false;
}

void WorkspacesView::tap(Target target) {
    switch (target.kind) {
        case Target::Kind::terminal:
            to_terminal({});
            break;
        case Target::Kind::make_new:
            // Typed, not run: the terminal shows what it takes (sqpg asks the rest).
            to_terminal("sqpg new ");
            break;
        case Target::Kind::refresh:
            static_cast<void>(stage_.telegraph().do_(addresses::refresh));
            break;
        case Target::Kind::card: {
            const Card& c = cards_[target.card];
            if (c.form == "compressed") {
                // Always exploded to work on (docs/design.md): beside home's others.
                const std::size_t dot = c.id.rfind('.');
                const std::string name = dot == std::string::npos ? c.id : c.id.substr(dot + 1);
                to_terminal("square explode " + shell_quoted(c.path) + " ~ && cd ~/" +
                            shell_quoted(name) + " && clear\r");
            } else {
                to_terminal("cd " + shell_quoted(c.path) + " && clear\r");
            }
            break;
        }
        case Target::Kind::none:
            break;
    }
}

void WorkspacesView::to_terminal(std::string_view text) {
    if (!text.empty()) {
        static_cast<void>(stage_.telegraph().do_(sqterm::addresses::input, text));
    }
    static_cast<void>(stage_.push<sqterm::Screen>());
}

// ---- the screen ---------------------------------------------------------------------------------

void WorkspacesScreen::build(sq::gui::LayoutWriter& root) {
    root.add_custom("workspaces_view").grow();
}

std::unique_ptr<sq::gui::Widget> make_workspaces(sq::Stage& stage) {
    return std::make_unique<WorkspacesView>(stage);
}

void open_workspaces(sq::Stage& stage) {
    static_cast<void>(stage.back());
}

}  // namespace sqide
