// sqIDE: the app. The squared SDK's IDE for the phone, with sqterm's terminal
// as its backbone (docs/design.md).
//
// This file is sqIDE's own: sqterm's main.cpp is replaced here, not merged
// (an upstream change to it is carried over by hand). Everything else
// sqterm's is used as it is.
//
// squared_main runs once, when the first window is up:
//
//   - the model: one, made of parts (sq::messaging::Composite, squared
//     1.2.0): sqterm's Shell (the shell in a pty), and sqIDE's Workspaces
//     (square's answer to find, for the cards), each on the model's thread;
//   - the screens: Workspaces, the root (the cards; a tap opens one in the
//     terminal), and sqterm's terminal, pushed over it;
//   - the drawer (the terminal's): sqIDE's name, and Workspaces, which goes
//     back to the cards;
//   - the background notice, as sqterm's: the shell keeps running.
//
// The prefix is sqterm's way, in sqIDE's own folder: the APK's packages are
// unpacked into it at the first start, relocated from sqterm's folder as they
// land (sq_app/src/unpack.cpp).

#include "ide/workspaces.hpp"
#include "sqterm.hpp"

namespace sqterm {

namespace {

/// The app's name: what the terminal, the notification and the drawer say.
constexpr std::string_view app_name = "sqIDE";

/// The one App of the process, named by squared_main (UI thread only).
sq::App* the_app = nullptr;

}  // namespace

void quit_app() noexcept {
    if (the_app != nullptr) {
        the_app->quit();
    }
}

void Screen::build(sq::gui::LayoutWriter& root) {
    root.add_custom("terminal_view").grow();
}

std::unique_ptr<sq::gui::Widget> make_terminal(sq::Stage& stage) {
    return std::make_unique<TerminalView>(stage, glyphs());
}

}  // namespace sqterm

namespace {

/// sqIDE's model: the shell, and what knows the workspaces.
using Model = sq::messaging::Composite<sqterm::Shell, sqide::Workspaces>;

}  // namespace

void squared_main(sq::App& app) {
    sqterm::the_app = &app;
    sqterm::Places places;
    places.name = std::string(sqterm::app_name);
    if (sq::files::FileSystem* files = app.stage.files()) {
        using sq::files::FileType;
        static_cast<void>(files->make_directories(FileType::Local, "home"));
        static_cast<void>(files->make_directories(FileType::Local, "usr/tmp"));
        places.home = files->resolve_platform_path(FileType::Local, "home");
        places.tmp = files->resolve_platform_path(FileType::Local, "usr/tmp");
        places.prefix = files->resolve_platform_path(FileType::Local, "usr");
        sqterm::stage_packages(*files, places);
    }
    sqterm::Places for_workspaces = places;
    for_workspaces.staged.clear();  // the Shell unpacks them; the cards need only where
    app.run<Model>(std::move(places), std::move(for_workspaces));
    static_cast<void>(app.keep_running({.title = sqterm::app_name, .text = "1 session"}));

    sqterm::set_drawer({.title = std::string(sqterm::app_name),
                        .buttons = {{.label = "Workspaces", .tap = &sqide::open_workspaces}}});
    // By their names: so Android can keep them.
    app.stage.add<sqide::WorkspacesScreen>();
    app.stage.add<sqterm::Screen>();
    app.stage.custom("terminal_view", &sqterm::make_terminal);
    app.stage.custom("workspaces_view", &sqide::make_workspaces);
    app.stage.on_surface(&sqterm::Glyphs::restore, &sqterm::glyphs());
    app.stage.push<sqide::WorkspacesScreen>();
}
