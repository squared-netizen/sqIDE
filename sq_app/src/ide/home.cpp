// sqIDE: the open workspace's Home (home.hpp says what is here).
//
// This file is sqIDE's own (sqterm has none of it).

#include "home.hpp"

#include "workspaces.hpp"

namespace sqide {

namespace {

constexpr float margin = 16.0F;
constexpr float line = 28.0F;

}  // namespace

HomeView::HomeView(sq::Stage& stage) noexcept : stage_(stage) {
    drawers_.set(screen_items(stage_),
                 {{"Terminal here",
                   [this] {
                       // The shell goes to the workspace, then the terminal shows.
                       if (!path_.empty()) {
                           const std::string cd = "cd " + shell_quoted(path_) + "\r";
                           static_cast<void>(stage_.telegraph().do_(sqterm::addresses::input,
                                                                    std::string_view(cd)));
                       }
                       go_terminal(stage_);
                   }},
                  {"Refresh", [this] {
                       if (!path_.empty()) {
                           static_cast<void>(
                               stage_.telegraph().do_(addresses::open, std::string_view(path_)));
                       }
                   }}});
}

sq::gui::Size HomeView::preferred_size(sq::gui::Painter& /*painter*/,
                                       const sq::gui::Skin& /*skin*/) const {
    return {320.0F, 480.0F};  // it grows to fill the screen
}

void HomeView::act(double delta_seconds) {
    sq::gui::Widget::act(delta_seconds);
    if (const auto shot = stage_.telegraph().snapshot(addresses::home);
        shot && (!have_home_ || shot->sequence != sequence_)) {
        sequence_ = shot->sequence;
        have_home_ = true;
        read_home({reinterpret_cast<const char*>(shot->bytes.data()), shot->bytes.size()});
    }
}

void HomeView::read_home(std::string_view json) {
    facts_.clear();
    ran_.clear();
    message_.clear();
    const auto parsed = sq::data::parse_json(json);
    if (!parsed) {
        message_ = "square's answer is not JSON";
        return;
    }
    const sq::data::JsonValue& home = parsed.value;
    if (const std::string opening = text_of(home.find("opening")); !opening.empty()) {
        message_ = "opening " + opening + " ...";
        return;
    }
    if (const sq::data::JsonValue* ran = home.find("ran");
        ran != nullptr && ran->array_if() != nullptr) {
        for (const auto& command : *ran->array_if()) {
            ran_.push_back("ran: " + text_of(&command));
        }
    }
    if (const std::string error = text_of(home.find("error")); !error.empty()) {
        message_ = error;
        return;
    }
    path_ = text_of(home.find("path"));
    const sq::data::JsonValue* info = home.find("info");
    if (info == nullptr) {
        return;
    }
    facts_.emplace_back("id", text_of(info->find("id")));
    facts_.emplace_back("version", text_of(info->find("version")));
    if (const sq::data::JsonValue* made = info->find("made_from"); made != nullptr) {
        if (std::string t = text_of(made->find("template")); !t.empty()) {
            facts_.emplace_back("template", std::move(t));
        }
        if (std::string k = text_of(made->find("kit")); !k.empty()) {
            facts_.emplace_back("kit", std::move(k));
        }
    }
    if (const sq::data::JsonValue* roles = info->find("roles");
        roles != nullptr && roles->object_if() != nullptr) {
        std::string names;
        for (const auto& [name, _] : *roles->object_if()) {
            names += (names.empty() ? "" : ", ") + name;
        }
        facts_.emplace_back("roles", std::move(names));
    }
    facts_.emplace_back("path", path_);
    std::string title = text_of(info->find("title"));
    facts_.insert(facts_.begin(),
                  {"", title.empty() ? text_of(info->find("id")) : std::move(title)});
}

void HomeView::paint(sq::gui::Painter& painter, const sq::gui::Skin& /*skin*/, float stage_x,
                     float stage_y) const {
    painter.fill_rectangle({stage_x, stage_y, width(), height()}, colour::background);
    const float x = stage_x + margin;
    float y = stage_y + margin;
    painter.draw_text("Home", x, y, colour::cyan);
    y += line * 1.5F;
    for (const auto& [label, value] : facts_) {
        if (label.empty()) {
            painter.draw_text(value, x, y, colour::text);  // the title
            y += line * 1.5F;
            continue;
        }
        painter.draw_text(label, x, y, colour::dim);
        painter.draw_text(value, x + 96.0F, y, colour::text);
        y += line;
    }
    if (!message_.empty()) {
        painter.draw_text(message_, x, y, colour::magenta);
        y += line;
    }
    // What was run to get here: the same commands work in the terminal.
    y = std::max(y + line,
                 stage_y + height() - (margin + (line * static_cast<float>(ran_.size()))));
    for (const std::string& r : ran_) {
        painter.draw_text(r, x, y, colour::dim);
        y += line;
    }
    drawers_.paint(painter, stage_x, stage_y, width(), height());
}

bool HomeView::pointer_event(const sq::gui::PointerEvent& event) {
    static_cast<void>(drawers_.pointer(event, width(), height()));
    return true;
}

void HomeScreen::build(sq::gui::LayoutWriter& root) {
    root.add_custom("home_view").grow();
}

std::unique_ptr<sq::gui::Widget> make_home(sq::Stage& stage) {
    return std::make_unique<HomeView>(stage);
}

}  // namespace sqide
