#pragma once
// [r]evo Mission in [r]evo Slicer: on the start screen the user picks Simple Mission (the slicer) or Mission
// Control (the slicer plus production). Mission Control fills the Mission menu and the start screen's production
// strip; both open the docked machine's Mission views (orders, queue and farm, clients, shops) in a separate window.

#include <string>

class wxMenu;
class wxWindow;

namespace Slic3r::GUI::Revo {

enum class MissionView { Orders, Queue, Clients, Shops };

bool mission_control();
void set_mission_control(bool on);

// Start screen (Main Launch Control): the path cards until the user picks one, and in Mission Control
// the production strip with the docked machine's summary. `refresh` fetches a new summary in the background.
void send_mission_home(bool refresh);
// A "homepage_mission" message from the start screen: start, switch, open <view>, or state.
void handle_home_message(const std::string& action, bool control, bool remember, const std::string& view);

// Rebuilds the Mission menu for the current path.
void fill_mission_menu(wxMenu* menu, wxWindow* parent);

void open_mission(MissionView view);

} // namespace Slic3r::GUI::Revo
