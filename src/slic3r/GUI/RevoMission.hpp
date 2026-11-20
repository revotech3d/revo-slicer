#pragma once
// [r]evo Mission in [r]evo Slicer: at startup the user picks Simple Mission (the slicer) or Mission Control
// (the slicer plus production). Mission Control adds the Mission menu, whose items open the docked
// machine's Mission views (orders, queue and farm, clients, shops) in a separate window.

class wxMenu;
class wxWindow;

namespace Slic3r::GUI::Revo {

enum class MissionView { Orders, Queue, Clients, Shops };

bool mission_control();
void set_mission_control(bool on);

// Startup: asks for the path unless the user ticked "Don't ask at startup".
void maybe_ask_start_path(wxWindow* parent);

// Rebuilds the Mission menu for the current path.
void fill_mission_menu(wxMenu* menu, wxWindow* parent);

void open_mission(MissionView view);

} // namespace Slic3r::GUI::Revo
