// sqIDE: the open workspace's Home (docs/design.md): what it is, and what was
// run to open it. Its right drawer has what Home does (Terminal here,
// Refresh); Build and Run come from square roles and sqmake describe next.
//
// This file is sqIDE's own (sqterm has none of it).

module;

#include "../sqterm.hpp"

export module sqide:home;

import :ui;

export namespace sqide {

/// The open workspace, drawn from the model's ws.home. UI thread only.
class HomeView final : public sq::gui::Widget {
public:
    explicit HomeView(sq::Stage& stage) noexcept;

    [[nodiscard]] sq::gui::Size preferred_size(sq::gui::Painter& painter,
                                               const sq::gui::Skin& skin) const override;
    void act(double delta_seconds) override;
    void paint(sq::gui::Painter& painter, const sq::gui::Skin& skin, float stage_x,
               float stage_y) const override;
    bool pointer_event(const sq::gui::PointerEvent& event) override;

private:
    void read_home(std::string_view json);

    sq::Stage& stage_;
    Drawers drawers_;
    std::uint64_t sequence_ = 0;
    bool have_home_ = false;
    std::string path_;
    /// What it is: label and value, from square info.
    std::vector<std::pair<std::string, std::string>> facts_;
    std::vector<std::string> ran_;  ///< "ran: square ..." lines
    std::string message_;           ///< opening, or square's error
};

/// The screen: over Workspaces, once one is open.
struct HomeScreen {
    static constexpr std::string_view name = "home";
    static void build(sq::gui::LayoutWriter& root);
};

/// The custom node's maker (Stage::custom).
std::unique_ptr<sq::gui::Widget> make_home(sq::Stage& stage);

}  // namespace sqide
