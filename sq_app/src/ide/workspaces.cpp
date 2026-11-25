// sqIDE: the Workspaces screen (workspaces.cppm says what is here).
//
// This file is sqIDE's own (sqterm has none of it).

module;

#include <stdio.h>  // popen, pclose
#include <sys/wait.h>

#include "../sqterm.hpp"

module sqide;

namespace sqide {

namespace {

constexpr float margin = 16.0F;
constexpr float cards_top = 56.0F;
constexpr float card_height = 84.0F;
constexpr float card_gap = 10.0F;
/// A finger that moves this far is scrolling, not tapping.
constexpr float drag_slop = 10.0F;

}  // namespace

std::string text_of(const sq::data::JsonValue* value) {
    const std::string* s = value != nullptr ? value->string_if() : nullptr;
    return s != nullptr ? *s : std::string();
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string shell_quoted(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

namespace {

void publish(sq::messaging::Telegraph& t, const sq::messaging::SnapshotKey& key,
             std::string_view text) {
    const std::span<std::byte> room = t.snapshot(key, text.size());
    if (room.size() < text.size()) {
        return;
    }
    std::memcpy(room.data(), text.data(), text.size());
    static_cast<void>(t.publish(key, text.size()));
}

}  // namespace

// ---- the model's part ---------------------------------------------------------------------------

Workspaces::Workspaces(sqterm::Places places) : places_(std::move(places)) {}

void Workspaces::start(sq::messaging::Telegraph& t) {
    find(t);
}

void Workspaces::receive(sq::messaging::Telegraph& t, const sq::messaging::Received& received) {
    const sq::messaging::Name to = received.telegram.address().name;
    if (to == addresses::refresh.name()) {
        find(t);
    } else if (to == addresses::open.name()) {
        open(t, std::string(reinterpret_cast<const char*>(received.data.data()),
                            received.data.size()));
    } else if (to == addresses::templates.name()) {
        templates(t);
    } else if (to == addresses::create.name()) {
        create(t, {reinterpret_cast<const char*>(received.data.data()), received.data.size()});
    }
}

std::pair<int, std::string> Workspaces::run(std::string_view program, std::string_view arguments,
                                            bool with_stderr) const {
    // The environment the terminal's commands have (HOME for sqpg's own
    // resources, PATH for what these run in turn), as NAME='value' words.
    std::string command;
    for (const std::string& e : environment(places_)) {
        const std::size_t eq = e.find('=');
        command += e.substr(0, eq + 1) + shell_quoted(e.substr(eq + 1)) + " ";
    }
    command += shell_quoted(places_.prefix + "/bin/" + std::string(program)) + " " +
               std::string(arguments) + (with_stderr ? " 2>&1" : " 2>/dev/null");
    std::string out;
    int status = -1;
    if (FILE* pipe = ::popen(command.c_str(), "r")) {
        std::array<char, 4096> buffer{};
        while (const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
            out.append(buffer.data(), n);
        }
        const int raw = ::pclose(pipe);
        status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    }
    return {status, out};
}

void Workspaces::find(sq::messaging::Telegraph& t) {
    auto [status, out] = square("--json find " + shell_quoted(places_.home));
    if (status != 0 || out.empty()) {
        out =
            "{\"error\": " +
            json_string(status == 127 || out.empty() ? "square is not installed: pkg install square"
                                                     : out) +
            "}";
    }
    publish(t, addresses::cards, out);
}

void Workspaces::open(sq::messaging::Telegraph& t, std::string path) {
    publish(t, addresses::home, "{\"opening\": " + json_string(path) + "}");
    std::string ran = "[";
    // A .sq is always exploded to work on: into home, beside the others.
    if (path.ends_with(".sq")) {
        const std::string args = "explode " + shell_quoted(path) + " " + shell_quoted(places_.home);
        ran += json_string("square " + args);
        const auto [status, out] = square("--json " + args);
        const auto parsed = sq::data::parse_json(out);
        const std::string exploded =
            parsed ? text_of(parsed.value.find("workspace")) : std::string();
        if (status != 0 || exploded.empty()) {
            publish(t, addresses::home,
                    "{\"error\": " + json_string(out) + ", \"ran\": " + ran + "]}");
            find(t);
            return;
        }
        path = exploded;
        ran += ", ";
    }
    const std::string args = "--workspace " + shell_quoted(path) + " info";
    ran += json_string("square " + args);
    const auto [status, out] = square("--json " + args);
    if (status != 0) {
        publish(t, addresses::home, "{\"error\": " + json_string(out) + ", \"ran\": " + ran + "]}");
        find(t);
        return;
    }
    // What its build can do: sqmake's verbs (build always; run, test,
    // install as the build file says) and the actions it names.
    const std::string request =
        R"({"sq": 1, "command": "plan", "args": {"dir": )" + json_string(path) + "}}";
    const std::string asked = places_.tmp + "/sqide-plan.json";
    if (std::ofstream file(asked, std::ios::trunc); file) {
        file << request;
    }
    const auto [planned, plan] = run("sqmake", "call < " + shell_quoted(asked), false);
    ran += ", " + json_string("sqmake call (plan)") + "]";
    std::string verbs = "[]";
    std::string actions = "[]";
    if (const auto p = sq::data::parse_json(plan); planned == 0 && p) {
        const sq::data::JsonValue* result = p.value.find("result");
        const sq::data::JsonValue& r = result != nullptr ? *result : p.value;
        if (const sq::data::JsonValue* v = r.find("verbs");
            v != nullptr && v->array_if() != nullptr) {
            verbs = "[";
            for (const auto& name : *v->array_if()) {
                verbs += (verbs.size() > 1 ? ", " : "") + json_string(text_of(&name));
            }
            verbs += "]";
        }
        if (const sq::data::JsonValue* a = r.find("actions");
            a != nullptr && a->array_if() != nullptr) {
            actions = "[";
            for (const auto& action : *a->array_if()) {
                actions +=
                    (actions.size() > 1 ? ", " : "") + json_string(text_of(action.find("name")));
            }
            actions += "]";
        }
    }
    publish(t, addresses::home,
            "{\"path\": " + json_string(path) + ", \"info\": " + out + ", \"verbs\": " + verbs +
                ", \"actions\": " + actions + ", \"ran\": " + ran + "}");
    find(t);  // the cards: a .sq exploded is a folder now too
}

void Workspaces::templates(sq::messaging::Telegraph& t) {
    const auto [status, out] = run("sqpg", "list template --json", false);
    const auto listed = sq::data::parse_json(out);
    if (status != 0 || !listed || listed.value.array_if() == nullptr) {
        publish(t, addresses::template_list,
                "{\"error\": " +
                    json_string(status == 127 || out.empty()
                                    ? "sqpg is not installed: pkg install squared-pg"
                                    : out) +
                    "}");
        return;
    }
    // Each with its parameters, which only `show` says.
    std::string list = "{\"templates\": [";
    bool first = true;
    for (const auto& item : *listed.value.array_if()) {
        const std::string id = text_of(item.find("id"));
        if (id.empty()) {
            continue;
        }
        const auto [shown, text] = run("sqpg", "show " + shell_quoted(id) + " --json", false);
        if (shown != 0 || !sq::data::parse_json(text)) {
            continue;
        }
        list += (first ? "" : ", ") + text;
        first = false;
    }
    publish(t, addresses::template_list, list + "]}");
}

namespace {

/// sqpg's error, said in a line: its message, and what it names.
std::string sqpg_error(const sq::data::JsonValue& answer) {
    const sq::data::JsonValue* error = answer.find("error");
    if (error == nullptr) {
        return "sqpg failed";
    }
    std::string said = text_of(error->find("message"));
    if (const sq::data::JsonValue* d = error->find("diagnostics")) {
        for (const char* list : {"missing", "invalid"}) {
            const sq::data::JsonValue* names = d->find(list);
            if (names == nullptr || names->array_if() == nullptr || names->array_if()->empty()) {
                continue;
            }
            said += std::string(" (") + list + ":";
            for (const auto& n : *names->array_if()) {
                said += " " + text_of(&n);
            }
            said += ")";
        }
    }
    return said;
}

}  // namespace

void Workspaces::create(sq::messaging::Telegraph& t, std::string_view request) {
    const auto parsed = sq::data::parse_json(request);
    const std::string id = parsed ? text_of(parsed.value.find("template")) : std::string();
    const std::string name = parsed ? text_of(parsed.value.find("name")) : std::string();
    if (id.empty() || name.empty()) {
        publish(t, addresses::created, R"({"error": "a template and a name, both", "ran": []})");
        return;
    }
    publish(t, addresses::created, "{\"creating\": " + json_string(name) + "}");
    std::string args = "new " + shell_quoted(name) + " --template " + shell_quoted(id) + " -o " +
                       shell_quoted(places_.home + "/" + name);
    if (const sq::data::JsonValue* params = parsed.value.find("params");
        params != nullptr && params->object_if() != nullptr) {
        for (const auto& [key, value] : *params->object_if()) {
            if (const std::string v = text_of(&value); !v.empty()) {
                args += " -p " + shell_quoted(key + "=" + v);
            }
        }
    }
    std::string ran = "[" + json_string("sqpg " + args);
    auto [status, out] = run("sqpg", args + " --json", false);
    auto answer = sq::data::parse_json(out);
    // A template that needs a kit says which: made again with them.
    if (status != 0 && answer) {
        const sq::data::JsonValue* error = answer.value.find("error");
        const sq::data::JsonValue* d = error != nullptr ? error->find("diagnostics") : nullptr;
        const sq::data::JsonValue* kits = d != nullptr ? d->find("required_kits") : nullptr;
        if (text_of(error != nullptr ? error->find("code") : nullptr) == "template.kit.required" &&
            kits != nullptr && kits->array_if() != nullptr) {
            for (const auto& k : *kits->array_if()) {
                args += " -k " + shell_quoted(text_of(&k));
            }
            ran += ", " + json_string("sqpg " + args);
            std::tie(status, out) = run("sqpg", args + " --json", false);
            answer = sq::data::parse_json(out);
        }
    }
    ran += "]";
    const std::string workspace = answer ? text_of(answer.value.find("workspace")) : std::string();
    if (status != 0 || workspace.empty()) {
        const std::string why =
            !answer ? (status == 127 || out.empty()
                           ? std::string("sqpg is not installed: pkg install squared-pg")
                           : out)
                    : sqpg_error(answer.value);
        publish(t, addresses::created,
                "{\"error\": " + json_string(why) + ", \"ran\": " + ran + "}");
        return;
    }
    publish(t, addresses::created,
            "{\"workspace\": " + json_string(workspace) + ", \"ran\": " + ran + "}");
    open(t, workspace);  // Home shows it, the cards have it
}

// ---- the widget ---------------------------------------------------------------------------------

WorkspacesView::WorkspacesView(sq::Stage& stage) noexcept : stage_(stage) {
    drawers_.set(
        screen_items(stage_),
        {{"New workspace", [this] { static_cast<void>(stage_.push<NewScreen>()); }},
         {"Refresh", [this] { static_cast<void>(stage_.telegraph().do_(addresses::refresh)); }}});
    // Shown again (back from Home): the cards as they are now.
    static_cast<void>(stage_.telegraph().do_(addresses::refresh));
}

sq::gui::Size WorkspacesView::preferred_size(sq::gui::Painter& /*painter*/,
                                             const sq::gui::Skin& /*skin*/) const {
    return {320.0F, 480.0F};  // it grows to fill the screen
}

void WorkspacesView::act(double delta_seconds) {
    sq::gui::Widget::act(delta_seconds);
    if (const auto shot = stage_.telegraph().snapshot(addresses::cards);
        shot && (!have_cards_ || shot->sequence != sequence_)) {
        sequence_ = shot->sequence;
        have_cards_ = true;
        read_cards({reinterpret_cast<const char*>(shot->bytes.data()), shot->bytes.size()});
    }
}

void WorkspacesView::read_cards(std::string_view json) {
    cards_.clear();
    message_.clear();
    const auto parsed = sq::data::parse_json(json);
    if (!parsed) {
        message_ = "square's answer is not JSON";
        return;
    }
    if (const std::string error = text_of(parsed.value.find("error")); !error.empty()) {
        message_ = error;
        return;
    }
    const sq::data::JsonValue* list = parsed.value.find("workspaces");
    if (list != nullptr && list->array_if() != nullptr) {
        for (const auto& w : *list->array_if()) {
            cards_.push_back({.path = text_of(w.find("path")),
                              .form = text_of(w.find("form")),
                              .id = text_of(w.find("id")),
                              .title = text_of(w.find("title")),
                              .version = text_of(w.find("version")),
                              .template_id = text_of(w.find("template"))});
        }
    }
    if (cards_.empty()) {
        message_ = "No workspaces yet. Swipe in from the right: New workspace.";
    }
}

sq::gui::Rectangle WorkspacesView::card(std::size_t i) const noexcept {
    return {margin, cards_top + scroll_ + (static_cast<float>(i) * (card_height + card_gap)),
            width() - (margin * 2.0F), card_height};
}

std::optional<std::size_t> WorkspacesView::card_at(float x, float y) const noexcept {
    if (y < cards_top) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        if (inside(card(i), x, y)) {
            return i;
        }
    }
    return std::nullopt;
}

void WorkspacesView::paint(sq::gui::Painter& painter, const sq::gui::Skin& /*skin*/, float stage_x,
                           float stage_y) const {
    painter.fill_rectangle({stage_x, stage_y, width(), height()}, colour::background);
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const sq::gui::Rectangle r = card(i);
        if (r.y + r.height < cards_top || r.y > height()) {
            continue;
        }
        const Card& c = cards_[i];
        const bool held = pressed_ == i && !dragging_;
        painter.fill_rectangle({stage_x + r.x, stage_y + r.y, r.width, r.height},
                               held ? colour::pressed : colour::card);
        // A stripe: cyan for a folder, magenta for a .sq (exploded when opened).
        painter.fill_rectangle({stage_x + r.x, stage_y + r.y, 4.0F, r.height},
                               c.form == "compressed" ? colour::magenta : colour::cyan);
        const float x = stage_x + r.x + 14.0F;
        const float y = stage_y + r.y + 10.0F;
        painter.draw_text(c.title.empty() ? c.id : c.title, x, y, colour::text);
        std::string second = c.id + "  " + c.version;
        if (!c.template_id.empty()) {
            second += "  ·  " + c.template_id;
        }
        if (c.form == "compressed") {
            second += "  ·  .sq";
        }
        painter.draw_text(second, x, y + 24.0F, colour::dim);
        painter.draw_text(c.path, x, y + 48.0F, colour::dim);
    }
    if (!message_.empty()) {
        painter.draw_text(message_, stage_x + margin, stage_y + cards_top + margin, colour::dim);
    }
    // The title over the cards that scroll under it.
    painter.fill_rectangle({stage_x, stage_y, width(), cards_top - 4.0F}, colour::background);
    painter.draw_text("Workspaces", stage_x + margin, stage_y + margin, colour::cyan);
    drawers_.paint(painter, stage_x, stage_y, width(), height());
}

bool WorkspacesView::pointer_event(const sq::gui::PointerEvent& event) {
    using sq::gui::PointerAction;
    if (drawers_.pointer(event, width(), height())) {
        pressed_.reset();
        return true;
    }
    switch (event.action) {
        case PointerAction::down:
            pressed_ = card_at(event.x, event.y);
            press_y_ = last_y_ = event.y;
            dragging_ = false;
            return true;
        case PointerAction::move: {
            if (!dragging_ && std::abs(event.y - press_y_) > drag_slop) {
                dragging_ = true;
            }
            if (dragging_) {
                const float content = static_cast<float>(cards_.size()) * (card_height + card_gap);
                const float lowest = std::min(0.0F, height() - cards_top - content - margin);
                scroll_ = std::clamp(scroll_ + (event.y - last_y_), lowest, 0.0F);
            }
            last_y_ = event.y;
            return true;
        }
        case PointerAction::up:
            if (!dragging_ && pressed_ && card_at(event.x, event.y) == pressed_) {
                // The model opens it (exploding a .sq first); Home shows it.
                static_cast<void>(stage_.telegraph().do_(addresses::open,
                                                         std::string_view(cards_[*pressed_].path)));
                set_workspace_open(true);
                static_cast<void>(stage_.push<HomeScreen>());
            }
            pressed_.reset();
            dragging_ = false;
            return true;
        case PointerAction::cancel:
            pressed_.reset();
            dragging_ = false;
            return true;
    }
    return false;
}

// ---- the screen ---------------------------------------------------------------------------------

void WorkspacesScreen::build(sq::gui::LayoutWriter& root) {
    root.add_custom("workspaces_view").grow();
}

std::unique_ptr<sq::gui::Widget> make_workspaces(sq::Stage& stage) {
    return std::make_unique<WorkspacesView>(stage);
}

}  // namespace sqide
