// sqIDE: the Workspaces screen (docs/design.md): every workspace under the
// home folder, as cards, newest first; tap one to open it in the terminal.
//
// This file is sqIDE's own (sqterm has none of it).
//
//   Workspaces      the model's part: asks square (`square --json find
//                   HOME`) at start and when asked again (ws.refresh), and
//                   publishes its answer, as square wrote it, as ws.cards
//   WorkspacesView  the widget: the cards, drawn; a tap opens one by typing
//                   the command into the terminal (cd, or square explode for
//                   a .sq), so the terminal shows what was done
//   WorkspacesScreen  the screen: the widget, filling it
//
// square is run as a program here, from the prefix (it is a package): the
// design links libsquare later, when sqIDE's build builds it.

#pragma once

#include "../sqterm.hpp"

namespace sqide {

namespace addresses {
/// Ask square again: the cards, as they are now.
inline constexpr sq::messaging::Key<void, sq::messaging::do_> refresh{"ws.refresh"};
/// square's answer to `find` (its JSON, as written), or {"error": "..."}.
inline constexpr sq::messaging::SnapshotKey cards{"ws.cards"};
}  // namespace addresses

/// The model's part that knows the workspaces (sq::messaging::Composite).
class Workspaces final {
public:
    static constexpr std::array takes{addresses::refresh.name()};

    explicit Workspaces(sqterm::Places places);

    void start(sq::messaging::Telegraph& t);
    void receive(sq::messaging::Telegraph& t, const sq::messaging::Received& received);

private:
    void find(sq::messaging::Telegraph& t);

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

private:
    struct Card {
        std::string path;
        std::string form;  ///< "exploded" or "compressed"
        std::string id;
        std::string title;
        std::string version;
        std::string template_id;
    };
    /// What can be tapped: a button at the top, or a card.
    struct Target {
        enum class Kind : std::uint8_t {
            none,
            terminal,
            make_new,
            refresh,
            card
        } kind = Kind::none;
        std::size_t card = 0;
        friend bool operator==(const Target&, const Target&) = default;
    };

    void read_cards(std::string_view json);
    [[nodiscard]] sq::gui::Rectangle button(std::size_t i) const noexcept;
    [[nodiscard]] sq::gui::Rectangle card(std::size_t i) const noexcept;
    [[nodiscard]] Target target_at(float x, float y) const noexcept;
    void tap(Target target);
    /// Type `text` into the shell and show the terminal.
    void to_terminal(std::string_view text);

    sq::Stage& stage_;
    std::uint64_t sequence_ = 0;
    bool have_cards_ = false;
    std::vector<Card> cards_;
    std::string message_;  ///< square's error, or "no workspaces"
    float scroll_ = 0.0F;
    Target pressed_;
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

/// The drawer's Workspaces button: back to the cards.
void open_workspaces(sq::Stage& stage);

}  // namespace sqide
