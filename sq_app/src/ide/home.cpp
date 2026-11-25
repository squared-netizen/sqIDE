// sqIDE: the open workspace's Home (home.cppm says what is here).
//
// This file is sqIDE's own (sqterm has none of it).

module;

#include "../sqterm.hpp"

module sqide;

namespace sqide {

namespace {

constexpr float margin = 16.0F;
constexpr float line = 26.0F;
constexpr float buttons_top = 112.0F;
constexpr float button_height = 48.0F;
constexpr float gap = 10.0F;
constexpr std::size_t columns = 2;
/// A finger that moves this far is scrolling, not tapping.
constexpr float drag_slop = 10.0F;

/// A verb's button: what it says.
std::string label_of(std::string_view verb) {
    if (verb == "build") {
        return "Build";
    }
    if (verb == "run") {
        return "Run";
    }
    if (verb == "test") {
        return "Test";
    }
    if (verb == "install") {
        return "Install";
    }
    return std::string(verb);
}

/// `text`, cut to fit `width` with an ellipsis.
std::string fit(sq::gui::Painter& painter, std::string text, float width) {
    if (painter.measure_text(text).width <= width) {
        return text;
    }
    while (!text.empty() && painter.measure_text(text + "…").width > width) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U) {
            text.pop_back();
        }
    }
    return text + "…";
}

}  // namespace

HomeView::HomeView(sq::Stage& stage) noexcept : stage_(stage), drawers_(stage) {
    drawers_.set(screen_items(stage_),
                 {{"Stop", [this] { static_cast<void>(stage_.telegraph().do_(addresses::stop)); }},
                  {"Terminal here",
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
    auto& t = stage_.telegraph();
    if (const auto shot = t.snapshot(addresses::home);
        shot && (!have_home_ || shot->sequence != home_sequence_)) {
        home_sequence_ = shot->sequence;
        have_home_ = true;
        read_home({reinterpret_cast<const char*>(shot->bytes.data()), shot->bytes.size()});
    }
    if (const auto shot = t.snapshot(addresses::output);
        shot && (!have_output_ || shot->sequence != output_sequence_)) {
        output_sequence_ = shot->sequence;
        have_output_ = true;
        read_output({reinterpret_cast<const char*>(shot->bytes.data()), shot->bytes.size()});
    }
}

void HomeView::read_home(std::string_view json) {
    buttons_.clear();
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
    if (const sq::data::JsonValue* info = home.find("info")) {
        title_ = text_of(info->find("title"));
        const std::string id = text_of(info->find("id"));
        if (title_.empty()) {
            title_ = id;
        }
        subtitle_ = id + "  " + text_of(info->find("version"));
        if (const sq::data::JsonValue* made = info->find("made_from")) {
            if (const std::string from = text_of(made->find("template")); !from.empty()) {
                subtitle_ += "  ·  " + from;
            }
        }
    }
    if (const sq::data::JsonValue* verbs = home.find("verbs");
        verbs != nullptr && verbs->array_if() != nullptr) {
        for (const auto& v : *verbs->array_if()) {
            const std::string verb = text_of(&v);
            // `sqmake` alone builds the defaults: build's own argv.
            buttons_.push_back({.label = label_of(verb),
                                .argv = verb == "build"
                                            ? std::vector<std::string>{"sqmake"}
                                            : std::vector<std::string>{"sqmake", verb}});
        }
    }
    if (const sq::data::JsonValue* actions = home.find("actions");
        actions != nullptr && actions->array_if() != nullptr) {
        for (const auto& a : *actions->array_if()) {
            const std::string name = text_of(&a);
            buttons_.push_back({.label = name, .argv = {"sqmake", name}});
        }
    }
    if (buttons_.empty()) {
        message_ = "Its build says nothing it can do (sqmake plan): see Terminal here";
    }
}

void HomeView::read_output(std::string_view json) {
    const auto parsed = sq::data::parse_json(json);
    if (!parsed) {
        return;
    }
    const sq::data::JsonValue& o = parsed.value;
    run_title_ = text_of(o.find("title"));
    run_command_ = text_of(o.find("command"));
    run_state_ = text_of(o.find("state"));
    if (const sq::data::JsonValue* s = o.find("status"); s != nullptr) {
        if (const std::int64_t* n = s->signed_integer_if()) {
            run_status_ = static_cast<int>(*n);
        } else if (const std::uint64_t* u = s->unsigned_integer_if()) {
            run_status_ = static_cast<int>(*u);
        }
    }
    output_ = text_of(o.find("output"));
    ++output_change_;
}

void HomeView::run(const Button& b) {
    if (path_.empty() || run_state_ == "running") {
        return;
    }
    std::string argv = "[";
    for (const std::string& a : b.argv) {
        argv += (argv.size() > 1 ? ", " : "") + json_string(a);
    }
    argv += "]";
    const std::string request = "{\"cwd\": " + json_string(path_) + ", \"argv\": " + argv +
                                ", \"title\": " + json_string(b.label) + "}";
    back_ = 0.0F;  // to the end of what it writes
    static_cast<void>(stage_.telegraph().do_(addresses::run, std::string_view(request)));
}

// ---- where things are
// -----------------------------------------------------------------------------

sq::gui::Rectangle HomeView::button(std::size_t i) const noexcept {
    const float w = (width() - (margin * 2.0F) - (gap * static_cast<float>(columns - 1))) /
                    static_cast<float>(columns);
    const auto row = static_cast<float>(i / columns);
    const auto column = static_cast<float>(i % columns);
    return {margin + (column * (w + gap)), buttons_top + (row * (button_height + gap)), w,
            button_height};
}

std::optional<std::size_t> HomeView::button_at(float x, float y) const noexcept {
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        if (inside(button(i), x, y)) {
            return i;
        }
    }
    return std::nullopt;
}

float HomeView::panel_top() const noexcept {
    const std::size_t rows = (buttons_.size() + columns - 1) / columns;
    return buttons_top + (static_cast<float>(rows) * (button_height + gap)) + gap;
}

const std::vector<std::string>& HomeView::wrapped(sq::gui::Painter& painter, float width) const {
    if (lines_change_ == output_change_ && lines_width_ == width) {
        return lines_;
    }
    lines_.clear();
    lines_change_ = output_change_;
    lines_width_ = width;
    // The panel's font is the app's (monospaced): a character's width, once.
    const float each = std::max(1.0F, painter.measure_text("MMMMMMMMMM").width / 10.0F);
    const auto most = static_cast<std::size_t>(std::max(8.0F, width / each));
    std::size_t start = 0;
    while (start <= output_.size()) {
        std::size_t end = output_.find('\n', start);
        if (end == std::string::npos) {
            end = output_.size();
        }
        std::string_view rest(output_.data() + start, end - start);
        do {
            std::size_t cut = std::min(most, rest.size());
            while (cut < rest.size() && cut > 0 &&
                   (static_cast<unsigned char>(rest[cut]) & 0xC0U) == 0x80U) {
                --cut;  // not inside a UTF-8 character
            }
            lines_.emplace_back(rest.substr(0, cut));
            rest.remove_prefix(cut);
        } while (!rest.empty());
        start = end + 1;
    }
    while (!lines_.empty() && lines_.back().empty()) {
        lines_.pop_back();
    }
    return lines_;
}

// ---- drawing
// --------------------------------------------------------------------------------------

void HomeView::paint(sq::gui::Painter& painter, const sq::gui::Skin& /*skin*/, float stage_x,
                     float stage_y) const {
    painter.fill_rectangle({stage_x, stage_y, width(), height()}, colour::background);
    const float x = stage_x + margin;
    const float text_width = width() - (margin * 2.0F);
    painter.draw_text("Home", x, stage_y + margin, colour::cyan);
    painter.draw_text(fit(painter, title_, text_width), x, stage_y + margin + 30.0F, colour::text);
    painter.draw_text(fit(painter, subtitle_, text_width), x, stage_y + margin + 56.0F,
                      colour::dim);

    const bool running = run_state_ == "running";
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const sq::gui::Rectangle r = button(i);
        const bool held = pressed_ == i;
        painter.fill_rectangle({stage_x + r.x, stage_y + r.y, r.width, r.height},
                               held ? colour::pressed : colour::card);
        painter.fill_rectangle({stage_x + r.x, stage_y + r.y + r.height - 3.0F, r.width, 3.0F},
                               running ? colour::dim : colour::cyan);
        const std::string label = fit(painter, buttons_[i].label, r.width - 16.0F);
        const sq::gui::Size size = painter.measure_text(label);
        painter.draw_text(label, stage_x + r.x + ((r.width - size.width) * 0.5F),
                          stage_y + r.y + ((r.height - size.height) * 0.5F),
                          running ? colour::dim : colour::text);
    }

    // The panel: the last run's state and output; before one, how Home was
    // opened.
    const float top = panel_top();
    painter.fill_rectangle({stage_x, stage_y + top, width(), height() - top}, colour::card);
    float y = stage_y + top + 8.0F;
    if (!message_.empty()) {
        painter.draw_text(fit(painter, message_, text_width), x, y, colour::magenta);
        y += line;
    }
    if (run_state_.empty()) {
        for (const std::string& r : ran_) {
            painter.draw_text(fit(painter, r, text_width), x, y, colour::dim);
            y += line;
        }
    } else {
        const std::string said = run_state_ == "running" ? "running"
                                 : run_state_ == "done"  ? "done"
                                 : run_state_ == "stopped"
                                     ? "stopped"
                                     : "failed (" + std::to_string(run_status_) + ")";
        const sq::graphics::Color c = run_state_ == "done"      ? colour::cyan
                                      : run_state_ == "running" ? colour::text
                                                                : colour::magenta;
        painter.draw_text(fit(painter, run_title_ + "  ·  " + said, text_width), x, y, c);
        y += line;
        painter.draw_text(fit(painter, "ran: " + run_command_, text_width), x, y, colour::dim);
        y += line;
        const std::vector<std::string>& lines = wrapped(painter, text_width);
        const float room = (stage_y + height()) - y - 8.0F;
        const auto fits = static_cast<std::size_t>(std::max(0.0F, room / line));
        const auto back = static_cast<std::size_t>(back_ / line);
        const std::size_t last = lines.size() > back ? lines.size() - back : 0;
        const std::size_t first = last > fits ? last - fits : 0;
        for (std::size_t i = first; i < last; ++i) {
            painter.draw_text(lines[i], x, y, colour::text);
            y += line;
        }
    }
    drawers_.paint(painter, stage_x, stage_y, width(), height());
}

// ---- touches
// --------------------------------------------------------------------------------------

bool HomeView::pointer_event(const sq::gui::PointerEvent& event) {
    using sq::gui::PointerAction;
    if (drawers_.pointer(event, width(), height())) {
        pressed_.reset();
        return true;
    }
    switch (event.action) {
        case PointerAction::down:
            pressed_ = run_state_ == "running" ? std::nullopt : button_at(event.x, event.y);
            in_panel_ = event.y >= panel_top();
            last_y_ = event.y;
            return true;
        case PointerAction::move:
            if (pressed_ && std::abs(event.y - last_y_) > drag_slop) {
                pressed_.reset();
            }
            if (in_panel_) {
                // Dragged down: back through what it wrote; up: to its end.
                const float most = static_cast<float>(lines_.size()) * line;
                back_ = std::clamp(back_ + (event.y - last_y_), 0.0F, std::max(0.0F, most));
                last_y_ = event.y;
            }
            return true;
        case PointerAction::up:
            if (pressed_ && button_at(event.x, event.y) == pressed_) {
                run(buttons_[*pressed_]);
            }
            pressed_.reset();
            in_panel_ = false;
            return true;
        case PointerAction::cancel:
            pressed_.reset();
            in_panel_ = false;
            return true;
    }
    return false;
}

void HomeScreen::build(sq::gui::LayoutWriter& root) {
    root.add_custom("home_view").grow();
}

std::unique_ptr<sq::gui::Widget> make_home(sq::Stage& stage) {
    return std::make_unique<HomeView>(stage);
}

}  // namespace sqide
