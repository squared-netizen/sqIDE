// sqIDE: New, the wizard (wizard.cppm says what is here).
//
// This file is sqIDE's own (sqterm has none of it).

module;

#include "../sqterm.hpp"

module sqide;

namespace sqide {

namespace {

constexpr float margin = 16.0F;
constexpr float top = 56.0F;  ///< below the title
constexpr float card_height = 84.0F;
constexpr float card_gap = 10.0F;
constexpr float field_label = 24.0F;  ///< the label, above its box
constexpr float field_height = 44.0F;
constexpr float field_step = 84.0F;  ///< one field to the next
constexpr float button_height = 52.0F;
constexpr float line = 26.0F;
/// A finger that moves this far is scrolling, not tapping.
constexpr float drag_slop = 10.0F;

/// `text`, cut to fit `width` with an ellipsis.
std::string fit(sq::gui::Painter& painter, std::string text, float width) {
    if (painter.measure_text(text).width <= width) {
        return text;
    }
    while (!text.empty() && painter.measure_text(text + "…").width > width) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U) {
            text.pop_back();  // a whole UTF-8 character at a time
        }
    }
    return text + "…";
}

}  // namespace

NewView::NewView(sq::Stage& stage) noexcept : stage_(stage), drawers_(stage) {
    drawers_.set(
        screen_items(stage_),
        {{"Templates", [this] { to(Step::pick); }},
         {"Refresh", [this] { static_cast<void>(stage_.telegraph().do_(addresses::templates)); }}});
    // An answer from before this wizard opened is not this one's.
    if (const auto shot = stage_.telegraph().snapshot(addresses::created)) {
        created_sequence_ = shot->sequence;
        have_created_ = true;
    }
    static_cast<void>(stage_.telegraph().do_(addresses::templates));
    message_ = "Asking sqpg for the templates ...";
}

sq::gui::Size NewView::preferred_size(sq::gui::Painter& /*painter*/,
                                      const sq::gui::Skin& /*skin*/) const {
    return {320.0F, 480.0F};  // it grows to fill the screen
}

void NewView::act(double delta_seconds) {
    sq::gui::Widget::act(delta_seconds);
    auto& t = stage_.telegraph();
    if (const auto shot = t.snapshot(addresses::template_list);
        shot && (!have_list_ || shot->sequence != list_sequence_)) {
        list_sequence_ = shot->sequence;
        have_list_ = true;
        read_templates({reinterpret_cast<const char*>(shot->bytes.data()), shot->bytes.size()});
    }
    if (const auto shot = t.snapshot(addresses::created);
        shot && (!have_created_ || shot->sequence != created_sequence_)) {
        created_sequence_ = shot->sequence;
        have_created_ = true;
        if (step_ == Step::creating) {
            read_created({reinterpret_cast<const char*>(shot->bytes.data()), shot->bytes.size()});
        }
    }
}

void NewView::read_templates(std::string_view json) {
    templates_.clear();
    const auto parsed = sq::data::parse_json(json);
    if (!parsed) {
        message_ = "sqpg's answer is not JSON";
        message_is_error_ = true;
        return;
    }
    if (const std::string error = text_of(parsed.value.find("error")); !error.empty()) {
        message_ = error;
        message_is_error_ = true;
        return;
    }
    const sq::data::JsonValue* list = parsed.value.find("templates");
    if (list != nullptr && list->array_if() != nullptr) {
        for (const auto& item : *list->array_if()) {
            Template t{.id = text_of(item.find("id")),
                       .title = text_of(item.find("title")),
                       .version = text_of(item.find("version")),
                       .description = text_of(item.find("description")),
                       .parameters = {}};
            if (const sq::data::JsonValue* ps = item.find("parameters");
                ps != nullptr && ps->array_if() != nullptr) {
                for (const auto& p : *ps->array_if()) {
                    Field f{.name = text_of(p.find("name")),
                            .label = text_of(p.find("name")),
                            .hint = text_of(p.find("description")),
                            .required = false,
                            .value = {}};
                    if (const sq::data::JsonValue* r = p.find("required")) {
                        f.required = r->boolean_if() != nullptr && *r->boolean_if();
                    }
                    // The workspace's name is project_name: asked for first.
                    if (f.name != "project_name" && !f.name.empty()) {
                        t.parameters.push_back(std::move(f));
                    }
                }
            }
            templates_.push_back(std::move(t));
        }
    }
    message_ = templates_.empty() ? "sqpg knows no templates" : std::string();
    message_is_error_ = templates_.empty();
}

void NewView::read_created(std::string_view json) {
    const auto parsed = sq::data::parse_json(json);
    if (!parsed) {
        return;
    }
    if (const std::string name = text_of(parsed.value.find("creating")); !name.empty()) {
        message_ = "Making " + name + " ...";
        return;
    }
    ran_.clear();
    if (const sq::data::JsonValue* ran = parsed.value.find("ran");
        ran != nullptr && ran->array_if() != nullptr) {
        for (const auto& command : *ran->array_if()) {
            ran_.push_back("ran: " + text_of(&command));
        }
    }
    if (const std::string error = text_of(parsed.value.find("error")); !error.empty()) {
        to(Step::form);
        message_ = error;
        message_is_error_ = true;
        return;
    }
    // Made, and being opened: Home, in New's place.
    set_workspace_open(true);
    stage_.replace<HomeScreen>();
}

void NewView::to(Step step) {
    step_ = step;
    scroll_ = 0.0F;
    pressed_.reset();
    pressed_create_ = false;
    if (active_) {
        active_.reset();
        stage_.show_keyboard(false);
    }
    if (step == Step::pick) {
        message_ = templates_.empty() ? message_ : std::string();
        message_is_error_ = templates_.empty() && message_is_error_;
        ran_.clear();
    }
}

void NewView::choose(std::size_t i) {
    chosen_ = i;
    fields_.clear();
    fields_.push_back({.name = "",
                       .label = "Name",
                       .hint = "a C identifier: its folder, and its program's name",
                       .required = true,
                       .value = {}});
    for (const Field& f : templates_[i].parameters) {
        fields_.push_back(f);
    }
    to(Step::form);
    message_.clear();
    message_is_error_ = false;
    active_ = 0;  // the name, at once
    stage_.show_keyboard(true);
}

void NewView::next_field() {
    if (!active_ || *active_ + 1 >= fields_.size()) {
        create();
        return;
    }
    active_ = *active_ + 1;
}

void NewView::create() {
    if (!chosen_ || fields_.empty()) {
        return;
    }
    for (const Field& f : fields_) {
        if (f.required && f.value.empty()) {
            message_ = f.label + " is needed";
            message_is_error_ = true;
            return;
        }
    }
    std::string request = "{\"template\": " + json_string(templates_[*chosen_].id) +
                          ", \"name\": " + json_string(fields_.front().value) + ", \"params\": {";
    bool first = true;
    for (std::size_t i = 1; i < fields_.size(); ++i) {
        if (fields_[i].value.empty()) {
            continue;  // the template's default
        }
        request += (first ? "" : ", ") + json_string(fields_[i].name) + ": " +
                   json_string(fields_[i].value);
        first = false;
    }
    request += "}}";
    to(Step::creating);
    message_ = "Making " + fields_.front().value + " ...";
    message_is_error_ = false;
    static_cast<void>(stage_.telegraph().do_(addresses::create, std::string_view(request)));
}

// ---- where things are
// -----------------------------------------------------------------------------

sq::gui::Rectangle NewView::card(std::size_t i) const noexcept {
    return {margin, top + scroll_ + (static_cast<float>(i) * (card_height + card_gap)),
            width() - (margin * 2.0F), card_height};
}

sq::gui::Rectangle NewView::box(std::size_t i) const noexcept {
    return {margin, top + scroll_ + (static_cast<float>(i) * field_step) + field_label,
            width() - (margin * 2.0F), field_height};
}

sq::gui::Rectangle NewView::create_button() const noexcept {
    return {margin, top + scroll_ + (static_cast<float>(fields_.size()) * field_step) + margin,
            width() - (margin * 2.0F), button_height};
}

std::optional<std::size_t> NewView::card_at(float x, float y) const noexcept {
    for (std::size_t i = 0; y >= top && i < templates_.size(); ++i) {
        if (inside(card(i), x, y)) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> NewView::box_at(float x, float y) const noexcept {
    for (std::size_t i = 0; y >= top && i < fields_.size(); ++i) {
        if (inside(box(i), x, y)) {
            return i;
        }
    }
    return std::nullopt;
}

float NewView::content_height() const noexcept {
    if (step_ == Step::pick) {
        return static_cast<float>(templates_.size()) * (card_height + card_gap);
    }
    return (static_cast<float>(fields_.size()) * field_step) + button_height +
           (static_cast<float>(ran_.size() + 2) * line) + (margin * 2.0F);
}

// ---- drawing
// --------------------------------------------------------------------------------------

void NewView::paint(sq::gui::Painter& painter, const sq::gui::Skin& /*skin*/, float stage_x,
                    float stage_y) const {
    painter.fill_rectangle({stage_x, stage_y, width(), height()}, colour::background);
    const float inner = width() - (margin * 2.0F) - 28.0F;
    if (step_ == Step::pick) {
        for (std::size_t i = 0; i < templates_.size(); ++i) {
            const sq::gui::Rectangle r = card(i);
            if (r.y + r.height < top || r.y > height()) {
                continue;
            }
            const Template& t = templates_[i];
            const bool held = pressed_ == i && !dragging_;
            painter.fill_rectangle({stage_x + r.x, stage_y + r.y, r.width, r.height},
                                   held ? colour::pressed : colour::card);
            painter.fill_rectangle({stage_x + r.x, stage_y + r.y, 4.0F, r.height}, colour::magenta);
            const float x = stage_x + r.x + 14.0F;
            const float y = stage_y + r.y + 10.0F;
            painter.draw_text(fit(painter, t.title.empty() ? t.id : t.title, inner), x, y,
                              colour::text);
            painter.draw_text(fit(painter, t.id + "  " + t.version, inner), x, y + 24.0F,
                              colour::dim);
            painter.draw_text(fit(painter, t.description, inner), x, y + 48.0F, colour::dim);
        }
    } else {
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            const sq::gui::Rectangle b = box(i);
            if (b.y + b.height < top || b.y - field_label > height()) {
                continue;
            }
            const Field& f = fields_[i];
            const bool editing = active_ == i;
            painter.draw_text(f.label + (f.required ? "" : "  (optional)"), stage_x + b.x,
                              stage_y + b.y - field_label + 2.0F,
                              editing ? colour::cyan : colour::dim);
            painter.fill_rectangle({stage_x + b.x, stage_y + b.y, b.width, b.height},
                                   editing ? colour::pressed : colour::card);
            painter.fill_rectangle({stage_x + b.x, stage_y + b.y + b.height - 2.0F, b.width, 2.0F},
                                   editing ? colour::cyan : colour::dim);
            const float tx = stage_x + b.x + 10.0F;
            const float ty = stage_y + b.y + 12.0F;
            if (f.value.empty() && !editing) {
                painter.draw_text(fit(painter, f.hint, b.width - 20.0F), tx, ty, colour::dim);
            } else {
                painter.draw_text(f.value, tx, ty, colour::text);
            }
            if (editing) {  // the cursor, at the end
                const float w = painter.measure_text(f.value).width;
                painter.fill_rectangle({tx + w + 1.0F, ty - 2.0F, 2.0F, 22.0F}, colour::cyan);
            }
        }
        const sq::gui::Rectangle c = create_button();
        const bool busy = step_ == Step::creating;
        painter.fill_rectangle({stage_x + c.x, stage_y + c.y, c.width, c.height},
                               pressed_create_ || busy ? colour::pressed : colour::cyan);
        const std::string label = busy ? "Making ..." : "Create";
        const sq::gui::Size size = painter.measure_text(label);
        painter.draw_text(label, stage_x + c.x + ((c.width - size.width) * 0.5F),
                          stage_y + c.y + ((c.height - size.height) * 0.5F),
                          busy ? colour::text : colour::background);
        float y = stage_y + c.y + c.height + margin;
        if (!message_.empty()) {
            painter.draw_text(fit(painter, message_, width() - (margin * 2.0F)), stage_x + margin,
                              y, message_is_error_ ? colour::magenta : colour::text);
            y += line;
        }
        for (const std::string& r : ran_) {
            painter.draw_text(fit(painter, r, width() - (margin * 2.0F)), stage_x + margin, y,
                              colour::dim);
            y += line;
        }
    }
    if (step_ == Step::pick && !message_.empty()) {
        painter.draw_text(message_, stage_x + margin, stage_y + top + margin,
                          message_is_error_ ? colour::magenta : colour::dim);
    }
    // The title over what scrolls under it.
    painter.fill_rectangle({stage_x, stage_y, width(), top - 4.0F}, colour::background);
    const std::string title =
        step_ == Step::pick || !chosen_
            ? std::string("New workspace")
            : "New: " + (templates_[*chosen_].title.empty() ? templates_[*chosen_].id
                                                            : templates_[*chosen_].title);
    painter.draw_text(fit(painter, title, width() - (margin * 2.0F)), stage_x + margin,
                      stage_y + margin, colour::cyan);
    drawers_.paint(painter, stage_x, stage_y, width(), height());
}

// ---- touches and keys
// -----------------------------------------------------------------------------

bool NewView::pointer_event(const sq::gui::PointerEvent& event) {
    using sq::gui::PointerAction;
    if (drawers_.pointer(event, width(), height())) {
        pressed_.reset();
        pressed_create_ = false;
        return true;
    }
    switch (event.action) {
        case PointerAction::down:
            press_y_ = last_y_ = event.y;
            dragging_ = false;
            if (step_ == Step::pick) {
                pressed_ = card_at(event.x, event.y);
            } else if (step_ == Step::form) {
                pressed_ = box_at(event.x, event.y);
                pressed_create_ = inside(create_button(), event.x, event.y);
            }
            return true;
        case PointerAction::move: {
            if (!dragging_ && std::abs(event.y - press_y_) > drag_slop) {
                dragging_ = true;
                pressed_create_ = false;
            }
            if (dragging_) {
                const float lowest = std::min(0.0F, height() - top - content_height() - margin);
                scroll_ = std::clamp(scroll_ + (event.y - last_y_), lowest, 0.0F);
            }
            last_y_ = event.y;
            return true;
        }
        case PointerAction::up:
            if (!dragging_) {
                if (step_ == Step::pick && pressed_ && card_at(event.x, event.y) == pressed_) {
                    choose(*pressed_);
                } else if (step_ == Step::form) {
                    if (pressed_create_ && inside(create_button(), event.x, event.y)) {
                        create();
                    } else if (pressed_ && box_at(event.x, event.y) == pressed_) {
                        active_ = pressed_;
                        stage_.show_keyboard(true);
                    } else if (active_) {
                        active_.reset();  // a tap beside the fields puts the keyboard away
                        stage_.show_keyboard(false);
                    }
                }
            }
            pressed_.reset();
            pressed_create_ = false;
            dragging_ = false;
            return true;
        case PointerAction::cancel:
            pressed_.reset();
            pressed_create_ = false;
            dragging_ = false;
            return true;
    }
    return false;
}

bool NewView::key_down(sq::app::Key key, sq::app::KeyModifiers /*modifiers*/) {
    using sq::app::Key;
    if (key == Key::escape) {
        to(Step::pick);
        return true;
    }
    if (step_ != Step::form || !active_) {
        return false;
    }
    switch (key) {
        case Key::backspace: {
            std::string& v = fields_[*active_].value;
            while (!v.empty() && (static_cast<unsigned char>(v.back()) & 0xC0U) == 0x80U) {
                v.pop_back();
            }
            if (!v.empty()) {
                v.pop_back();
            }
            return true;
        }
        case Key::enter:
        case Key::tab:
            next_field();
            return true;
        default:
            return false;
    }
}

bool NewView::text_input(std::string_view text) {
    if (step_ != Step::form || !active_ || text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c == '\n' || c == '\r') {
            next_field();  // a keyboard that sends Enter as text
            return true;
        }
    }
    fields_[*active_].value += text;
    return true;
}

// ---- the screen
// -----------------------------------------------------------------------------------

void NewScreen::build(sq::gui::LayoutWriter& root) {
    root.add_custom("new_view").grow();
}

std::unique_ptr<sq::gui::Widget> make_new(sq::Stage& stage) {
    return std::make_unique<NewView>(stage);
}

}  // namespace sqide
