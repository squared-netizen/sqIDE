# sqIDE: its screens, and how they hand off

Status: design, for review. Nothing here is built yet; the terminal, sqgui's
edit commands (0.3, 0.4) and the relocating unpacker are.

## What sqIDE is

The squared SDK's IDE for the phone: sqterm's terminal as its backbone, with
three screens of its own around it:

| Screen     | What it is for                                                    |
|------------|-------------------------------------------------------------------|
| Landing    | your workspaces: open one, make one (`sqpg new`), the plain shell |
| Terminal   | sqterm's, unchanged: the shell in the workspace you opened        |
| Settings   | the theme's colours, the font's size, the keyboard                |
| Editor     | a layout of the workspace, edited by touch (sqgui's edits)        |

The terminal is the backbone: everything the other screens do is something
you could have typed (`square`, `sqgui set`, `sqpg new`), and each screen
says the command it ran, so the terminal teaches the commands as you go.

## Two rules from the repository

1. **Terminal changes are made in sqterm first** and merged here
   (`git merge upstream/main`). sqIDE's own code goes in new files, so the
   merges stay clean.
2. **`packages/`, `squared/` and the files packages are built from stay as
   sqterm's** (a package's key is a hash of its recipe's inputs: change one
   and sqterm's release has no archive for it). Those files are
   `sq_app/pkg/`, `sq_app/cli/`, `sq_app/src/unpack*`, and what
   `packages/*/recipe.sh` names in `inputs=`. New files are fine.

So sqIDE's code lives in `sq_app/src/ide/` (the build globs `sq_app/src/**`),
and the few places sqterm's files need a way in are added to sqterm first,
as extension points that do nothing in sqterm (below).

## Screens and the stack

squared's Stage keeps a stack of screens (`push`, `replace`, `back`) and
panes over the top one (`open`). sqIDE's stack:

```
launch ─► Landing  (the root: replace)
            │
            ├─ open a workspace ─► push Terminal   (cd there, its name in the title)
            ├─ plain shell ──────► push Terminal   (as it was left)
            ├─ New ─────────────► open pane "new workspace" (name, template) ─► push Terminal
            ├─ Edit UI (a row) ─► push Editor     (that workspace's layouts)
            └─ Settings ────────► push Settings

Terminal ── drawer ─┬─ Workspaces ─► back to Landing (the shell keeps running)
                    ├─ Edit UI ────► push Editor  (the workspace the shell is in)
                    ├─ Settings ───► push Settings
                    └─ Keyboard   (sqterm's, as now)

Editor ── its tool row ─► Terminal ─► push Terminal (the same session)
Back, on any screen ─► the screen beneath; on Landing, Android's Back (leave)
```

- **One shell session**, made when the app starts, kept while you move
  between screens. Opening a workspace types `cd 'PATH' && clear` into it,
  as you would; it does not start another shell. (Several sessions are a
  later step, sqterm's to add first.)
- **The terminal never leaves the stack below a screen it opened**, so Back
  from the Editor or Settings lands where you were.
- **Landing is the root, not the terminal**, so a cold start shows your
  workspaces. A setting (Settings, "Start in": Workspaces / Terminal) can
  make the terminal the root instead, for people who live in the shell.
- Panes, not screens, for the small things: New workspace, a confirm, the
  Editor's properties sheet.

## The model: one, made of parts

squared runs one model per app (`App::run`), on its own thread. sqIDE's is a
composite, `ide::Model`, that owns each part and hands each telegram to the
part whose address it is:

| Part          | Addresses   | Does                                                       |
|---------------|-------------|------------------------------------------------------------|
| `Shell`       | `term.*`    | sqterm's, unchanged: the pty, the screen snapshot           |
| `Workspaces`  | `ws.*`      | the list (from square), New (sqpg), which one is open      |
| `Editor`      | `edit.*`    | one layout file: its text, sqgui's edits, undo, the check |
| `Settings`    | `set.*`     | theme.json, the font size, the keyboard; tells the view    |

Each part is a class with sqterm's Shell's shape (`receive`, `readable`,
`publish`), so `ide::Model` is a dispatcher of about a page: by the first
word of the address. `Shell` needs nothing new for this.

How the parts reach the SDK's tools:

- **square: linked**, through libsquare (`square::call` takes and returns
  JSON: `info`, `resources`, `show`, `verify`). No process, no parsing of
  words. Workspaces needs one thing square does not have yet: **`square
  find DIR`**, the workspaces under a folder (each `SQ-INF/manifest.json`
  of kind workspace), for the landing list. That goes into square first.
- **sqgui: linked**, its modules `sqgui.reading` (compile and check, the
  build's own words) and `sqgui.edit` (set, unset, insert, remove, move:
  every other byte kept). The Editor and `sqgui` on the command line make
  the same edits and keep the same undo steps.
- **sqpg: run**, as a process in the shell's environment (it is a program
  with its own prompts); its output goes to the terminal, so New ends in
  the terminal, in the new workspace.

## What sqterm gets first (extension points)

Small, and each does nothing in sqterm itself:

1. **The drawer's buttons from a list** the app gives (`TerminalView`):
   sqterm gives `{Keyboard}`; sqIDE gives `{Workspaces, Edit UI, Settings,
   Keyboard}`, each a tap the view sends on (`stage.back()`, a push).
2. **`term.type`**: text typed into the shell by the app, not the keyboard
   (`cd 'PATH' && clear`), so the session's scroll position and the input
   echo behave as typing does.
3. **`term.theme`**: read theme.json again and repaint, so Settings shows a
   colour as you pick it. sqterm can use it too (a later Settings there).

## Settings

What it sets, and where it is kept (the files sqterm already reads):

| Setting                 | Kept in                          | Applied             |
|-------------------------|----------------------------------|---------------------|
| Theme colours (16 + 4)  | `~/.config/sqterm/theme.json`    | `term.theme`, live  |
| Font size               | `files/terminal.font` (as now)   | live                |
| Keyboard on start       | a new line in the same settings  | next start          |
| Start in                | `~/.config/sqide/settings.json`  | next start          |

The colour picker is the palette row from the Editor's design (swatches),
with a field for a hex value; "Reset" puts back cyberpunk dream.

## The Editor

Portrait, as decided (the palette on top, as the pixel editor does):

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
  rows light up as targets; the drop is `insert`. **Drag a widget** to move
  it: `move`. **Delete**: `remove`. **Handles** set `fill_x`, a weight, a
  span (layouts are rows of cells, not free positions: a handle snaps to
  those few values), each a `set` or `unset`.
- **Properties** (a pane, from the bottom): the selected widget's fields;
  `bind` and `tap` offer the addresses in names.json.
- **Tree**: `sqgui show`'s outline, to select what is hard to touch.
- **Every edit is checked** as the build checks it; one that gives the
  layout a problem is not saved, and the bar says why in the build's words.
- **The status line says the command** each edit was (`sqgui insert
  settings rows[1][0] {...}`).

The preview needs one thing squared has not got: **drawing a compiled layout
into a rectangle, scaled, without making it the Stage's screen** (a
`gui::LayoutView` widget: the layout's nodes, laid out at the target's size,
drawn through a transform; taps go to the Editor, not to the layout). That
is squared's to add, in the package, before the Editor.

## Order

1. sqterm: the drawer's list, `term.type`, `term.theme` (one PR); merged
   here.
2. square: `find DIR`; a package.
3. sqIDE: `ide::Model` and the parts' skeleton; the Landing screen; the
   drawer's buttons. The first APK where sqIDE is more than sqterm.
4. Settings.
5. squared: `gui::LayoutView`.
6. The Editor: preview and selection first, then the palette and drag and
   drop, then properties.

## Open questions

- Several shell sessions (tabs) before or after the Editor?
- Does a workspace remember the layout last edited (square's records) or
  does sqIDE keep that?
- The Editor in landscape on a phone: worth it, or portrait only at first?
