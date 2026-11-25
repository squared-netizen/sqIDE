// sqIDE: the Workspaces screen (docs/design.md): every workspace under the
// home folder, as cards, newest first; tap one to open it.
//
// This file is sqIDE's own (sqterm has none of it).
//
//   Workspaces      the model's part: asks square for the cards (`square
//                   --json find HOME`) at start and on ws.refresh, and opens
//                   one on ws.open: a .sq is exploded first (`square explode`,
//                   always: one is never worked on packed), then `square
//                   info` says what it is. The cards as ws.cards, the open
//                   one as ws.home, each with the commands it ran. For New:
//                   the templates sqpg knows (ws.templates, each with its
//                   parameters), and `sqpg new` (ws.create), which opens the
//                   workspace it made.
//   WorkspacesView  the widget: the cards; a tap opens one and shows Home.
//                   The left drawer moves between screens, the right one has
//                   this screen's: New, Refresh.
//   WorkspacesScreen  the screen: the root of sqIDE's stack.
//
// The terminal is not needed for any of it: the model runs square and sqpg
// itself, as programs from the prefix (they are packages); the design
// links libsquare later, when sqIDE's build builds it.

module;

#include "../sqterm.hpp"

export module sqide:workspaces;

import :ui;

export namespace sqide {

namespace addresses {
/// Ask square again: the cards, as they are now.
inline constexpr sq::messaging::Key<void, sq::messaging::do_> refresh{"ws.refresh"};
/// Open the workspace at this path (a folder, or a .sq to explode first).
inline constexpr sq::messaging::Key<std::string_view, sq::messaging::do_> open{"ws.open"};
/// square's answer to `find` (its JSON, as written), or {"error": "..."}.
inline constexpr sq::messaging::SnapshotKey cards{"ws.cards"};
/// The open workspace: {"path", "info" (square info's), "ran": [commands]}, or
/// {"error", "ran"}; {"opening": path} while it is being opened.
inline constexpr sq::messaging::SnapshotKey home{"ws.home"};
/// Ask sqpg for the templates, each with its parameters (`sqpg show`).
inline constexpr sq::messaging::Key<void, sq::messaging::do_> templates{"ws.templates"};
/// {"templates": [sqpg show's, ...]}, or {"error": "..."}.
inline constexpr sq::messaging::SnapshotKey template_list{"ws.template_list"};
/// Make a workspace: {"template": ID, "name": NAME, "params": {KEY: VALUE}}.
inline constexpr sq::messaging::Key<std::string_view, sq::messaging::do_> create{"ws.create"};
/// What became of it: {"workspace": path, "ran": [...]} (then ws.home has
/// it, opened), or {"error": "...", "ran": [...]}; {"creating": NAME} first.
inline constexpr sq::messaging::SnapshotKey created{"ws.created"};
}  // namespace addresses

/// The model's part that knows the workspaces (sq::messaging::Composite).
class Workspaces final {
public:
    static constexpr std::array takes{addresses::refresh.name(), addresses::open.name(),
                                      addresses::templates.name(), addresses::create.name()};

    explicit Workspaces(sqterm::Places places);

    void start(sq::messaging::Telegraph& t);
    void receive(sq::messaging::Telegraph& t, const sq::messaging::Received& received);

private:
    void find(sq::messaging::Telegraph& t);
    void open(sq::messaging::Telegraph& t, std::string path);
    void templates(sq::messaging::Telegraph& t);
    void create(sq::messaging::Telegraph& t, std::string_view request);
    /// A program from the prefix's bin/ with `arguments` (each quoted): its
    /// exit status and what it wrote: stderr too, so its error says why,
    /// unless it answers in JSON on stdout alone (sqpg --json).
    [[nodiscard]] std::pair<int, std::string> run(std::string_view program,
                                                  std::string_view arguments,
                                                  bool with_stderr = true) const;
    [[nodiscard]] std::pair<int, std::string> square(std::string_view arguments) const {
        return run("square", arguments);
    }

    sqterm::Places places_;
};

/// The cards, drawn; a tap on one opens it. UI thread only.
class WorkspacesView final : public sq::gui::Widget {
public:
    explicit WorkspacesView(sq::Stage& stage) noexcept;

    [[nodiscard]] sq::gui::Size preferred_size(sq::gui::Painter& painter,
                                               const sq::gui::Skin& skin) const override;
    void act(double delta_seconds) override;
    void paint(sq::gui::Painter& painter, const sq::gui::Skin& skin, float stage_x,
               float stage_y) const override;
    bool pointer_event(const sq::gui::PointerEvent& event) override;
    /// Two fingers tapped: the keyboard (the drawers' Keyboard button too).
    bool pinch(const sq::gui::PinchEvent& event) override { return drawers_.pinch(event); }
    /// Focusable and taking text, so the keyboard can be shown on it; it
    /// comes with Keyboard or two fingers, never with focus.
    [[nodiscard]] bool focusable() const noexcept override { return true; }
    [[nodiscard]] bool wants_text_input() const noexcept override { return true; }
    [[nodiscard]] bool keyboard_on_focus() const noexcept override { return false; }

private:
    struct Card {
        std::string path;
        std::string form;  ///< "exploded" or "compressed"
        std::string id;
        std::string title;
        std::string version;
        std::string template_id;
    };

    void read_cards(std::string_view json);
    [[nodiscard]] sq::gui::Rectangle card(std::size_t i) const noexcept;
    [[nodiscard]] std::optional<std::size_t> card_at(float x, float y) const noexcept;

    sq::Stage& stage_;
    Drawers drawers_;
    std::uint64_t sequence_ = 0;
    bool have_cards_ = false;
    std::vector<Card> cards_;
    std::string message_;  ///< square's error, or "no workspaces"
    float scroll_ = 0.0F;
    std::optional<std::size_t> pressed_;
    float press_y_ = 0.0F;
    float last_y_ = 0.0F;
    bool dragging_ = false;
};

/// The screen: the root of sqIDE's stack.
struct WorkspacesScreen {
    static constexpr std::string_view name = "workspaces";
    static void build(sq::gui::LayoutWriter& root);
};

/// The custom node's maker (Stage::custom).
std::unique_ptr<sq::gui::Widget> make_workspaces(sq::Stage& stage);

/// `s` in single quotes for the shell, each ' as '\''.
[[nodiscard]] std::string shell_quoted(std::string_view s);
/// `s` as a JSON string, quotes and all.
[[nodiscard]] std::string json_string(std::string_view s);
/// A JSON value's text, or "" (missing, or not a string).
[[nodiscard]] std::string text_of(const sq::data::JsonValue* value);

}  // namespace sqide
