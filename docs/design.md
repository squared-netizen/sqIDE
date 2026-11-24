# sqIDE: its screens, and how they hand off

Status: design, for review. Nothing here is built yet; the terminal, sqgui's
edit commands (0.3, 0.4), the relocating unpacker and squared's
`messaging::Composite` (1.2.0) are.

## What sqIDE is

The squared SDK's IDE for the phone, built so that the friendly way in never
walls off the deep end:

- **Discoverable.** Each screen shows what a workspace can do, as buttons,
  so nothing has to be known in advance.
- **Nothing hidden.** Every screen says the command it ran (`ran: sqgui
  insert settings rows[1] ...`), the terminal is one tap away on every
  screen, in the workspace's folder, and every button is also an nvim
  command and a macro (below). The GUI teaches the CLI as you use it.

| Screen          | What it is for                                                       |
|-----------------|----------------------------------------------------------------------|
| Workspaces      | the root: every workspace, as cards; open one, make one, clone one  |
| Workspace home  | one workspace: what it can do, as buttons, from what it is          |
| Code            | nvim, the code only; its windows as full-screen tabs; a tool drawer |
| GUI editor      | a layout of the workspace, edited by touch (sqgui's edits)           |
| Terminal        | sqterm's: the shell, in the workspace you opened                     |
| Settings        | the theme's colours, the font's size, the keyboard                   |

## Workspaces: cartridges made from templates

**Every workspace is a squared cartridge**, exploded (a folder) or
compressed (one `.sq` file), and every workspace is made from a template.
The template is what a workspace is; there are no kinds built into sqIDE:

| Template                | Makes                                                          |
|-------------------------|----------------------------------------------------------------|
| app templates           | a program of any sort: android (GUI), terminal, term.edu, ...  |
| the kit template        | a kit                                                          |
| the package template    | a package                                                      |
| the template template   | a template: users author their own, and it shows up in New     |

The workspace's `SQ-INF/manifest.json` says which template made it.

- **A compressed workspace is always exploded to work on**: opening a
  `.sq` card explodes it into the workspaces folder first (square does it;
  the card says so). **Export** packs a workspace back into one `.sq` to
  share. (square: `explode`, `pack`.)
- **New** is a short wizard: the template (every template square can find,
  your own included), a name, then that template's options; it runs `sqpg
  new` and says so.
- **Clone**: a git URL, then the same as opening.

## The workspace home: from what the workspace is

Not a screen per kind: the home is made from two answers, so a template
nobody has written yet gets a working home with no change to sqIDE.

- **square's roles** for the workspace (`square roles`): which buttons
  appear. A layouts role brings **GUI editor**; kits, **Kits**; sources,
  **Code**; a recipe (a package), **Recipe**.
- **sqmake's verbs and actions** (`sqmake describe`): a button each, **Run**,
  **Install**, **Test**, and whatever the template's build file adds
  (`sqmake packages`, a kit's "try it in a test app").

Always there: **Terminal** (here), **Problems** (the last build's, and
`square verify`'s conflicts), **Export**.

## Screens and the stack

squared's Stage keeps a stack of screens (`push`, `replace`, `back`) and
panes over the top one (`open`):

```
launch ─► Workspaces (the root)
            ├─ open (exploding a .sq first) ─► push Home
            ├─ New ─► pane: template, name, options ─► push Home
            └─ Clone ─► pane: URL ─► push Home

Home ─┬─ Code ─────► push Code
      ├─ GUI editor ► push GUI editor
      ├─ Run, Install, Test, an action ─► the terminal, running it
      └─ Terminal ──► push Terminal (cd here)

Back, on any screen ─► the screen beneath; on Workspaces, Android's Back
```

- **One shell session**, made when the app starts, kept while you move
  between screens; opening a workspace types `cd 'PATH' && clear` into it.
  (Several sessions are sqterm's to add first.)
- Panes, not screens, for the small things: New, Clone, a confirm, the GUI
  editor's properties sheet.

## The two drawers

- **The left drawer, on every screen** (a swipe from the left edge, as
  sqterm's now): Workspaces, this workspace's Home, Settings, Terminal,
  Keyboard.
- **The right drawer, on the Code screen**: a tree of buttons:

  ```
  ▸ Files      tree · recent · search
  ▸ Build      build · run · install · problems
  ▸ Debug      start · breakpoints · step · variables     (lldb-dap: the lldb package)
  ▸ Git        status · diff · commit · log
  ▸ Symbols    outline · go to · references               (clangd, from square intel)
  ▸ Names      names.json's addresses ↔ layouts ↔ handlers
  ```

**One definition for both drawers and nvim**: the tree is a Lua file,
`actions.lua` (people write Lua), each entry a name and a command. A drawer
button, an nvim command (`:SqBuild`) and a macro run the same thing. sqIDE
ships a default; yours adds to it or changes it.

## The Code screen

**Only code.** nvim fills the screen; every other nvim window (help, the
quickfix list, a file tree, a terminal, git) opens as its own full-screen
tab, and the tabs are a strip to swipe along. The tools are in the right
drawer and in nvim's own commands, never beside the code.

In two stages:

- **Stage A (first): nvim in the terminal view, and a plugin,
  `sqide.nvim`**, a package (offline, as every package is). It makes each
  new window a tab page (so it covers the screen), draws the tab strip, and
  gives the commands and keys `actions.lua` names. It is put on nvim's
  runtime path, so your own `init.lua` still has the last word. sqIDE asks
  nvim things through its RPC socket (`nvim --listen`): open a file at a
  line (a problem tapped), the buffer shown, save all before a build.
- **Stage B (after the GUI editor): a real nvim frontend**, through nvim's
  UI protocol (`nvim --embed`, multigrid, tabline, popup menu, command
  line). Each nvim window is its own grid, drawn by sqIDE as a native
  full-screen pane (squared's terminal grid draws the cells); completion is
  a list for fingers; `:` is a text field with its history.

**What nvim knows of the workspace, from square:**

- **`square intel`** writes `compile_commands.json` (and `.clangd`: the
  module paths for `import std` and squared's modules) at the workspace's
  root, from sqmake's plan (`sqmake call`, `plan`: every target and its
  command). square runs it after it makes or changes a workspace (New, a
  kit added) and when the Code screen opens a workspace whose build files
  changed; it is square's file, shown locked.
- **The file tree by role, not by folder** (`square roles`): Model, UI,
  Platform, Kits, Generated. What square generates and the framework's
  files show a lock and open read-only: the ownership classes, seen.
- **Packages** for clangd (llvm-tools, if it is not there yet), nvim-dap and
  a file tree, so all of it works offline.

## Where the two editors meet: names.json

The GUI editor knows a workspace from the UI's side, the Code screen from the
model's; names.json is the contract between them. In the GUI editor, a
widget's `bind: settings.volume` has **Go to model** (nvim, where the model
takes that name: the generated `names.hpp` symbol finds it). In the Code
screen, **Names** lists the addresses and the layouts using each (`sqgui
names`), and an address the model never takes is marked. A name added on
either side is in names.json, and both see it.

## The model: one, made of parts

squared runs one model an app (`App::run`); sqIDE's is
`sq::messaging::Composite<Shell, Workspaces, Build, Code, Editor, Settings>`
(squared 1.2.0): each part says the names it `takes`, and each telegram goes
to its part.

| Part          | Does                                                                   |
|---------------|------------------------------------------------------------------------|
| `Shell`       | sqterm's: the pty, the screen snapshot                                 |
| `Workspaces`  | the cards (square find), New (sqpg), explode and pack, the one open    |
| `Build`       | sqmake through its JSON (`sqmake call`): build, plan; the problems     |
| `Code`        | nvim's RPC socket: open at a line, the buffer shown, save all          |
| `Editor`      | one layout file: sqgui's edits, undo, the check                        |
| `Settings`    | theme.json, the font's size, the keyboard; tells the view             |

How they reach the SDK's tools: **square linked** (libsquare,
`square::call`: JSON in and out), **sqgui linked** (`sqgui.reading`,
`sqgui.edit`: the CLI's edits and undo), **sqmake run** (`sqmake call`, JSON
on its standard input and output), **sqpg run** in the terminal (it asks
questions).

## Two rules from the repository

1. **Terminal changes are made in sqterm first** and merged here
   (`git merge upstream/main`). sqIDE's own code goes in new files, so the
   merges stay clean.
2. **`packages/`, `squared/` and the files packages are built from stay as
   sqterm's** (a package's key is a hash of its recipe's inputs: change one
   and sqterm's release has no archive for it). Those are `sq_app/pkg/`,
   `sq_app/cli/`, `sq_app/src/unpack*`, and what `packages/*/recipe.sh`
   names in `inputs=`.

So sqIDE's code lives in `sq_app/src/ide/`, and the few places sqterm's
files need a way in are added to sqterm first, doing nothing there.

## What other repositories give first

- **sqterm**: the left drawer's buttons from a list the app gives;
  `term.type` (text typed into the shell by the app); `term.theme` (theme.json
  again, repainted); Shell's `takes` and `watches` (a Composite part).
- **square**: `find DIR`, `roles`, `intel`, `explode`, `pack`.
- **squared-pg**: the template template (and the kit and package
  templates, if they are not there yet).
- **squared**: `gui::LayoutView` (a compiled layout drawn into a rectangle,
  scaled; taps to the GUI editor), before the GUI editor.
- **packages**: `sqide-nvim` (the plugin, `actions.lua`'s default), nvim-dap,
  a file tree; clangd if llvm-tools lacks it.

## Settings

| Setting                 | Kept in                          | Applied             |
|-------------------------|----------------------------------|---------------------|
| Theme colours (16 + 4)  | `~/.config/sqterm/theme.json`    | `term.theme`, live  |
| Font size               | `files/terminal.font` (as now)   | live                |
| Keyboard on start       | `~/.config/sqide/settings.json`  | next start          |
| Start in                | `~/.config/sqide/settings.json`  | next start          |

The colour picker is the swatch row of the GUI editor's palette, with a field
for a hex value; Reset puts back cyberpunk dream.

## The GUI editor

Portrait, the palette on top (as the pixel editor does):

```
┌─────────────────────────────────┐
│ [lbl][btn][sld][tgl][txt][img]… │  widget palette, two rows, scrolls
│ [row][stk][scr][lst][pane]…     │
├─────────────────────────────────┤
│        live preview, scaled     │  the selected widget outlined, handles
│ (✥ pan)                (🔍 zoom) │  over it
├──── settings.json · rows[1][0] ─┤  the file, the selected path
│ [select][move][size][dup][del]  │  tools
│ [props][tree][check][undo][⌨ ]  │  ⌨: the terminal
└─────────────────────────────────┘
```

Landscape: the palette on the left, the tools on the right.

- **Drag from the palette** into the preview: the gaps between cells and
  rows light up as targets; the drop is `insert`. **Drag a widget**: `move`.
  **Delete**: `remove`. **Handles** set `fill_x`, a weight, a span (layouts
  are rows of cells: a handle snaps to those few values), each a `set` or
  `unset`.
- **Properties** (a pane): the selected widget's fields; `bind` and `tap`
  offer names.json's addresses, and **Go to model**.
- **Tree**: `sqgui show`'s outline, to select what is hard to touch.
- **Every edit is checked** as the build checks it; one that gives the
  layout a problem is not saved, and the bar says why in the build's words.

## Order

1. sqterm: the drawer's list, `term.type`, `term.theme`, Shell's `takes`
   and `watches`; merged here, with squared 1.2.0.
2. square: `find`, `roles`, `explode`, `pack`; then `intel`.
3. sqIDE: the Composite model and its parts' skeleton; Workspaces, the
   Home, the left drawer. The first APK where sqIDE is more than sqterm.
4. The Code screen, stage A: `sqide.nvim`, the right drawer, `actions.lua`,
   Build and Problems.
5. Settings.
6. squared: `gui::LayoutView`; then the GUI editor.
7. The Code screen, stage B: the nvim frontend.

## Decided

- The workspace home: yes (2026-11-24).
- Kinds: none in sqIDE; templates make everything, a template template
  included; every workspace a cartridge, a compressed one always exploded
  to work on.
- The Code screen: only code; nvim's windows as full-screen tabs; the
  tools in a right drawer (a tree of buttons) and in nvim's commands and
  macros, one definition (`actions.lua`).
- `compile_commands.json`: square's (`square intel`), not sqmake's.
- Stage A now; stage B after the GUI editor.

## Open questions

- Several shell sessions (tabs) before or after the GUI editor?
- The GUI editor in landscape on a phone: worth it, or portrait only at
  first?
