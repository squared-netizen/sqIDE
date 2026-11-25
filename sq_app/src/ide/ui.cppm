// sqIDE: what every screen of its own shares (docs/design.md, "The two
// drawers"): the colours, the navigation, and the drawers.
//
// This file is sqIDE's own (sqterm has none of it).
//
//   The left drawer, on every screen, moves between screens: Workspaces,
//   Home (once a workspace is open), Terminal. A swipe in from the left edge.
//   The right drawer holds what the screen itself does (New, Refresh;
//   Terminal here, Build, Run). A swipe in from the right edge.
//
// sqterm's terminal keeps its own drawer, from the left (set_drawer gives it
// the same screens); its keyboard button is the terminal's own.

module;

#include "../sqterm.hpp"

export module sqide:ui;

export namespace sqide {

/// The colours: the theme's (cyberpunk dream), as the terminal draws them.
namespace colour {
inline constexpr sq::graphics::Color background{
    .red = 0.043F, .green = 0.027F, .blue = 0.098F, .alpha = 1.0F};
inline constexpr sq::graphics::Color card{
    .red = 0.106F, .green = 0.063F, .blue = 0.208F, .alpha = 1.0F};
inline constexpr sq::graphics::Color pressed{
    .red = 0.18F, .green = 0.11F, .blue = 0.33F, .alpha = 1.0F};
inline constexpr sq::graphics::Color text{
    .red = 0.90F, .green = 0.88F, .blue = 1.0F, .alpha = 1.0F};
inline constexpr sq::graphics::Color dim{
    .red = 0.62F, .green = 0.58F, .blue = 0.78F, .alpha = 1.0F};
inline constexpr sq::graphics::Color cyan{
    .red = 0.02F, .green = 0.85F, .blue = 0.91F, .alpha = 1.0F};
inline constexpr sq::graphics::Color magenta{
    .red = 1.0F, .green = 0.16F, .blue = 0.43F, .alpha = 1.0F};
inline constexpr sq::graphics::Color shade{
    .red = 0.0F, .green = 0.0F, .blue = 0.0F, .alpha = 0.45F};
}  // namespace colour

[[nodiscard]] inline bool inside(const sq::gui::Rectangle& r, float x, float y) noexcept {
    return r.width > 0.0F && x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

// ---- navigation: Workspaces (the root), then Home, then a tool over it -------------------------

/// Whether a workspace is open (Home is under the tool shown, or shown). UI thread.
[[nodiscard]] bool workspace_open() noexcept;
void set_workspace_open(bool open) noexcept;

void go_workspaces(sq::Stage& stage);  ///< back to the root
void go_home(sq::Stage& stage);        ///< the open workspace's Home
void go_terminal(sq::Stage& stage);    ///< the terminal, over Home (or the root)

/// The left drawer's screens, for sqterm's drawer too (set_drawer).
[[nodiscard]] std::vector<sqterm::DrawerButton> screens();

// ---- the drawers --------------------------------------------------------------------------------

/// One drawer's button: what it says, and what a tap does (UI thread).
struct Item {
    std::string label;
    std::function<void()> tap;
};

/// The left drawer of sqIDE's own screens: Workspaces, Home, Terminal.
[[nodiscard]] std::vector<Item> screen_items(sq::Stage& stage);

/// The two drawers of a screen: painted over it, and taking the touches
/// that open, use and close them. A widget owns one, and asks it first.
///
/// The left drawer ends, at its bottom left, with Keyboard (as Termux's
/// does), on every screen; two fingers tapped anywhere do the same, as on
/// the terminal. The widget that owns the drawers says it takes text
/// (wants_text_input), or the keyboard is not shown for it.
class Drawers {
public:
    explicit Drawers(sq::Stage& stage) noexcept : stage_(stage) {}

    /// The left drawer's items (the screens), and the right's (this screen's).
    void set(std::vector<Item> left, std::vector<Item> right);

    /// A touch the drawers take: true when they did (the widget ignores it).
    bool pointer(const sq::gui::PointerEvent& event, float width, float height);
    /// Two fingers: tapped, the keyboard up or away. True when it was a tap.
    bool pinch(const sq::gui::PinchEvent& event);
    void paint(sq::gui::Painter& painter, float x0, float y0, float width, float height) const;
    [[nodiscard]] bool open() const noexcept { return side_ != Side::none; }

private:
    enum class Side : std::uint8_t { none, left, right };
    [[nodiscard]] sq::gui::Rectangle pane(Side side, float width, float height) const noexcept;
    [[nodiscard]] sq::gui::Rectangle item(Side side, std::size_t i, float width,
                                          float height) const noexcept;
    [[nodiscard]] const std::vector<Item>& items(Side side) const noexcept;
    /// The left drawer's Keyboard button, at its bottom left.
    [[nodiscard]] sq::gui::Rectangle keyboard(float width, float height) const noexcept;

    sq::Stage& stage_;
    std::vector<Item> left_;
    std::vector<Item> right_;
    Side side_ = Side::none;
    Side edge_ = Side::none;  ///< a press by an edge: may swipe that drawer in
    float press_x_ = 0.0F;
    std::optional<std::size_t> held_;
    bool held_keyboard_ = false;
    std::chrono::steady_clock::time_point pinch_began_{};
    bool pinch_moved_ = false;
};

}  // namespace sqide
