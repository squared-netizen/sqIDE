// sqIDE: its own code, as one module (sqterm's stays as sqterm.hpp, shared
// with sqterm as it is). Every screen of sqIDE's own is a part of it:
//
//   :ui          the colours, the navigation, the drawers
//   :workspaces  the Workspaces screen, and the model's part that knows them
//   :home        the open workspace's Home
//   :wizard      New: a template, its name and options, then sqpg new
//
// This file is sqIDE's own (sqterm has none of it).

export module sqide;

export import :ui;
export import :workspaces;
export import :home;
export import :wizard;
