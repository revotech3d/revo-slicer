#pragma once
// [r]evo Mission in [r]evo Slicer: on the start screen the user picks Simple Mission (the slicer) or Mission
// Control (the slicer plus production). Mission Control adds the MISSION tab, first before Prepare: orders, clients
// and finished prints are kept by the slicer on this computer (data_dir()/revo_mission), so production is managed
// without any printer; order items go to any added printer, online or offline, and finished prints come back from
// the printers' agents to be attached to an order, a client or own production.

#include <string>

class wxMenu;
class wxWindow;

namespace Slic3r::GUI::Revo {

enum class MissionView { Unassigned, Assigned, Completed, Prints, Printers, Clients };

bool mission_control();
void set_mission_control(bool on);

// Start screen (Main Launch Control): the path cards until the user picks one, and in Mission Control the production
// strip with the summary the MISSION page saved last.
void send_mission_home(bool refresh);
// A "homepage_mission" message from the start screen: start, switch, open <view>, or state.
void handle_home_message(const std::string& action, bool control, bool remember, const std::string& view);

// Rebuilds the Mission menu for the current path.
void fill_mission_menu(wxMenu* menu, wxWindow* parent);

// Page of the MISSION tab; MainFrame inserts it while Mission Control is on.
wxWindow* create_mission_page(wxWindow* parent);

void open_mission(MissionView view);

// Printer states changed: the MISSION tab updates its printers and queues.
void mission_printer_changed();

} // namespace Slic3r::GUI::Revo
