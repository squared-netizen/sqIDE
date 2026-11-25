// sqIDE: New, the wizard that makes a workspace (docs/design.md): the
// template, then its name and options, then Create, which runs `sqpg new`
// and opens what it made on its Home.
//
// This file is sqIDE's own (sqterm has none of it).
//
//   NewView    the widget, in three steps:
//                pick      the templates sqpg knows, as cards (yours too:
//                          what `sqmake install` put in your resources)
//                form      the name, then each of the template's parameters;
//                          a tap on a field brings the keyboard, Enter goes
//                          to the next, and from the last makes it
//                creating  what sqpg is doing; its error back on the form,
//                          or Home, with the workspace open
//              The left drawer moves between screens, the right one has
//              this screen's: Templates (back to the cards), Refresh.
//   NewScreen  the screen, over Workspaces.
//
// The model does the work (Workspaces: ws.templates, ws.create); the
// "ran:" lines say what it ran, so the same can be typed in the terminal.

module;

#include "../sqterm.hpp"

export module sqide:wizard;

import :ui;

export namespace sqide {

/// The wizard, drawn. UI thread only.
class NewView final : public sq::gui::Widget {
public:
    explicit NewView(sq::Stage& stage) noexcept;

    [[nodiscard]] sq::gui::Size preferred_size(sq::gui::Painter& painter,
                                               const sq::gui::Skin& skin) const override;
    void act(double delta_seconds) override;
    void paint(sq::gui::Painter& painter, const sq::gui::Skin& skin, float stage_x,
               float stage_y) const override;
    bool pointer_event(const sq::gui::PointerEvent& event) override;
    /// Two fingers tapped: the keyboard (the drawers' Keyboard button too).
    bool pinch(const sq::gui::PinchEvent& event) override { return drawers_.pinch(event); }
    bool key_down(sq::app::Key key, sq::app::KeyModifiers modifiers) override;
    bool text_input(std::string_view text) override;
    /// Focusable and taking text (or the Ui shows no keyboard for it); the
    /// keyboard comes with a tap on a field, not with focus.
    [[nodiscard]] bool focusable() const noexcept override { return true; }
    [[nodiscard]] bool wants_text_input() const noexcept override { return true; }
    [[nodiscard]] bool keyboard_on_focus() const noexcept override { return false; }

private:
    enum class Step : std::uint8_t { pick, form, creating };

    struct Field {
        std::string name;  ///< the parameter's ("" for the workspace's name)
        std::string label;
        std::string hint;
        bool required = false;
        std::string value;
    };
    struct Template {
        std::string id;
        std::string title;
        std::string version;
        std::string description;
        std::vector<Field> parameters;
    };

    void read_templates(std::string_view json);
    void read_created(std::string_view json);
    void choose(std::size_t i);
    void create();
    void to(Step step);
    void next_field();

    [[nodiscard]] sq::gui::Rectangle card(std::size_t i) const noexcept;
    [[nodiscard]] sq::gui::Rectangle box(std::size_t i) const noexcept;
    [[nodiscard]] sq::gui::Rectangle create_button() const noexcept;
    [[nodiscard]] std::optional<std::size_t> card_at(float x, float y) const noexcept;
    [[nodiscard]] std::optional<std::size_t> box_at(float x, float y) const noexcept;
    [[nodiscard]] float content_height() const noexcept;

    sq::Stage& stage_;
    Drawers drawers_;
    Step step_ = Step::pick;
    std::uint64_t list_sequence_ = 0;
    bool have_list_ = false;
    std::uint64_t created_sequence_ = 0;
    bool have_created_ = false;
    std::vector<Template> templates_;
    std::optional<std::size_t> chosen_;
    std::vector<Field> fields_;  ///< the name, then the parameters
    std::optional<std::size_t> active_;
    std::string message_;
    bool message_is_error_ = false;
    std::vector<std::string> ran_;
    float scroll_ = 0.0F;
    std::optional<std::size_t> pressed_;
    bool pressed_create_ = false;
    float press_y_ = 0.0F;
    float last_y_ = 0.0F;
    bool dragging_ = false;
};

/// The screen: New, over Workspaces.
struct NewScreen {
    static constexpr std::string_view name = "new";
    static void build(sq::gui::LayoutWriter& root);
};

/// The custom node's maker (Stage::custom).
std::unique_ptr<sq::gui::Widget> make_new(sq::Stage& stage);

}  // namespace sqide
