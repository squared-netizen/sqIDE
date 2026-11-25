// sqIDE: the navigation and the drawers (ui.cppm says what they are).
//
// This file is sqIDE's own (sqterm has none of it).

module;

#include "../sqterm.hpp"

module sqide;

namespace sqide {

namespace {

constexpr float drawer_width = 280.0F;
constexpr float margin = 16.0F;
constexpr float item_height = 52.0F;
/// A press this near an edge may swipe that edge's drawer in, this far.
constexpr float edge = 32.0F;
constexpr float swipe = 40.0F;

bool open_workspace = false;

}  // namespace

bool workspace_open() noexcept {
    return open_workspace;
}

void set_workspace_open(bool open) noexcept {
    open_workspace = open;
}

// The stack: Workspaces at 1, Home at 2, a tool (the terminal) at 3. A
// change waits for the frame, and depth() counts what waits.
void go_workspaces(sq::Stage& stage) {
    while (stage.depth() > 1) {
        if (!stage.back()) {
            break;
        }
    }
}

void go_home(sq::Stage& stage) {
    if (!open_workspace) {
        return;
    }
    if (stage.depth() == 1) {
        static_cast<void>(stage.push<HomeScreen>());  // back to it from the cards
        return;
    }
    while (stage.depth() > 2) {
        if (!stage.back()) {
            break;
        }
    }
}

void go_terminal(sq::Stage& stage) {
    if (stage.showing<sqterm::Screen>()) {
        return;
    }
    if (stage.depth() > 2) {
        stage.replace<sqterm::Screen>();  // one tool at a time, over Home
    } else {
        static_cast<void>(stage.push<sqterm::Screen>());
    }
}

std::vector<sqterm::DrawerButton> screens() {
    return {{.label = "Workspaces", .tap = &go_workspaces}, {.label = "Home", .tap = &go_home}};
}

std::vector<Item> screen_items(sq::Stage& stage) {
    // Home is there always; with no workspace open it does nothing.
    return {{"Workspaces", [&stage] { go_workspaces(stage); }},
            {"Home", [&stage] { go_home(stage); }},
            {"Terminal", [&stage] { go_terminal(stage); }}};
}

// ---- the drawers --------------------------------------------------------------------------------

void Drawers::set(std::vector<Item> left, std::vector<Item> right) {
    left_ = std::move(left);
    right_ = std::move(right);
}

const std::vector<Item>& Drawers::items(Side side) const noexcept {
    return side == Side::right ? right_ : left_;
}

sq::gui::Rectangle Drawers::pane(Side side, float width, float height) const noexcept {
    const float w = std::min(width * 0.75F, drawer_width);
    return {side == Side::right ? width - w : 0.0F, 0.0F, w, height};
}

sq::gui::Rectangle Drawers::item(Side side, std::size_t i, float width,
                                 float height) const noexcept {
    const sq::gui::Rectangle p = pane(side, width, height);
    return {p.x + margin, (margin * 4.0F) + (static_cast<float>(i) * (item_height + margin)),
            p.width - (margin * 2.0F), item_height};
}

bool Drawers::pointer(const sq::gui::PointerEvent& event, float width, float height) {
    using sq::gui::PointerAction;
    const auto item_at = [&](float x, float y) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < items(side_).size(); ++i) {
            if (inside(item(side_, i, width, height), x, y)) {
                return i;
            }
        }
        return std::nullopt;
    };
    if (side_ != Side::none) {
        switch (event.action) {
            case PointerAction::down:
                press_x_ = event.x;
                held_ = item_at(event.x, event.y);
                break;
            case PointerAction::move:
                // Swiped back out, the way it came in.
                if ((side_ == Side::left && event.x - press_x_ < -swipe) ||
                    (side_ == Side::right && event.x - press_x_ > swipe)) {
                    side_ = Side::none;
                    held_.reset();
                } else if (held_ && item_at(event.x, event.y) != held_) {
                    held_.reset();
                }
                break;
            case PointerAction::up:
                if (held_ && item_at(event.x, event.y) == held_) {
                    const std::function<void()> tap = items(side_)[*held_].tap;
                    side_ = Side::none;
                    held_.reset();
                    if (tap) {
                        tap();
                    }
                } else if (!inside(pane(side_, width, height), event.x, event.y)) {
                    side_ = Side::none;  // a tap beside it puts it away
                }
                held_.reset();
                break;
            case PointerAction::cancel:
                held_.reset();
                break;
        }
        return true;
    }
    switch (event.action) {
        case PointerAction::down:
            press_x_ = event.x;
            edge_ = event.x < edge ? Side::left : event.x > width - edge ? Side::right : Side::none;
            return false;
        case PointerAction::move:
            if (edge_ == Side::left && event.x - press_x_ > swipe && !left_.empty()) {
                side_ = Side::left;
            } else if (edge_ == Side::right && press_x_ - event.x > swipe && !right_.empty()) {
                side_ = Side::right;
            }
            if (side_ != Side::none) {
                edge_ = Side::none;
                return true;
            }
            return edge_ != Side::none;  // a swipe in from an edge is the drawers'
        case PointerAction::up:
        case PointerAction::cancel:
            edge_ = Side::none;
            return false;
    }
    return false;
}

void Drawers::paint(sq::gui::Painter& painter, float x0, float y0, float width,
                    float height) const {
    if (side_ == Side::none) {
        return;
    }
    painter.fill_rectangle({x0, y0, width, height}, colour::shade);
    const sq::gui::Rectangle p = pane(side_, width, height);
    painter.fill_rectangle({x0 + p.x, y0 + p.y, p.width, p.height}, colour::card);
    painter.draw_text(side_ == Side::left ? "sqIDE" : "Here", x0 + p.x + margin, y0 + margin,
                      colour::cyan);
    const std::vector<Item>& list = items(side_);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const sq::gui::Rectangle r = item(side_, i, width, height);
        painter.fill_rectangle({x0 + r.x, y0 + r.y, r.width, r.height},
                               held_ == i ? colour::pressed : colour::background);
        const sq::gui::Size size = painter.measure_text(list[i].label);
        painter.draw_text(list[i].label, x0 + r.x + 14.0F,
                          y0 + r.y + ((r.height - size.height) * 0.5F), colour::text);
    }
}

}  // namespace sqide
