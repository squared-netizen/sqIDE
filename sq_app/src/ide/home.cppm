// sqIDE: the open workspace's Home (docs/design.md): what it is, a button
// for each thing its build can do, and what the last one wrote.
//
// This file is sqIDE's own (sqterm has none of it).
//
//   The buttons: sqmake's verbs for this build file (Build always; Run,
//   Test, Install as it says) and its actions, from ws.home. A tap runs
//   `sqmake VERB` in the workspace through the Runner, which does not hold
//   the model up; its output fills the panel below as it comes, and stays
//   at its end unless it is dragged back.
//   The right drawer: Stop (the one running), Terminal here, Refresh.

module;

#include "../sqterm.hpp"

export module sqide:home;

import :ui;

export namespace sqide {

/// The open workspace, drawn from the model's ws.home and run.output. UI
/// thread only.
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
    struct Button {
        std::string label;
        std::vector<std::string> argv;
    };

    void read_home(std::string_view json);
    void read_output(std::string_view json);
    void run(const Button& button);
    [[nodiscard]] sq::gui::Rectangle button(std::size_t i) const noexcept;
    [[nodiscard]] std::optional<std::size_t> button_at(float x, float y) const noexcept;
    [[nodiscard]] float panel_top() const noexcept;
    /// The output, cut into lines that fit `width` (kept until it changes).
    const std::vector<std::string>& wrapped(sq::gui::Painter& painter, float width) const;

    sq::Stage& stage_;
    Drawers drawers_;
    std::uint64_t home_sequence_ = 0;
    bool have_home_ = false;
    std::uint64_t output_sequence_ = 0;
    bool have_output_ = false;

    std::string path_;
    std::string title_;
    std::string subtitle_;  ///< id, version, what it was made from
    std::vector<Button> buttons_;
    std::vector<std::string> ran_;  ///< "ran: square ..." lines, from opening it
    std::string message_;           ///< opening, or square's error

    std::string run_title_;
    std::string run_command_;
    std::string run_state_;  ///< "" (none yet), running, done, failed, stopped
    int run_status_ = 0;
    std::string output_;
    std::uint64_t output_change_ = 0;  ///< counts changes, for the wrap's cache

    float back_ = 0.0F;  ///< how far the output is dragged back from its end
    std::optional<std::size_t> pressed_;
    bool in_panel_ = false;
    float last_y_ = 0.0F;

    mutable std::vector<std::string> lines_;
    mutable std::uint64_t lines_change_ = ~std::uint64_t{0};
    mutable float lines_width_ = 0.0F;
};

/// The screen: over Workspaces, once one is open.
struct HomeScreen {
    static constexpr std::string_view name = "home";
    static void build(sq::gui::LayoutWriter& root);
};

/// The custom node's maker (Stage::custom).
std::unique_ptr<sq::gui::Widget> make_home(sq::Stage& stage);

}  // namespace sqide
