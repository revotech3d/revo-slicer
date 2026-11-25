#pragma once
// [r]evo Slicer – kreatory:
//  - Newbie: jak najmniej pytań (wzór: prostota Stratasysa) – materiał, jakość, wytrzymałość, podpory i gotowe.
//  - Monek: kalibracja nowego filamentu krok po kroku (temperatura, przepływ, PA, współczynnik, retrakcja) i zapis profilu.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { struct Calib_Params; }

class wxWindow;

namespace Slic3r::GUI::Revo {

void show_newbie_wizard(wxWindow* parent);
void show_monek_wizard(wxWindow* parent);
// Połączenie z drukarką Revon: adres + login i PIN kierownika → klucz API z maszyny → zapisana drukarka z wysyłaniem.
void show_connect_wizard(wxWindow* parent);

// „My printers” na ekranie startowym: dodane maszyny (profil z adresem) ze stanem z sieci; klik = aktywna drukarka.
void send_my_printers(bool refresh = true);
void select_my_printer(const std::string& name);
// The same list for [r]evo Mission: name, host, model, state from the last network check.
nlohmann::json my_printers_json();
// Presentation mode: the sample machines shown instead of My printers (names used by the sample orders).
std::vector<std::string> demo_printer_names();
// Added printers with their address and slicer API key (for [r]evo Mission's calls to the agent).
struct PrinterLink { std::string name, host, key; };
std::vector<PrinterLink> printer_links();
// Checks the machines in the background; the start screen and [r]evo Mission get the new states.
void refresh_printers();
void remove_my_printer(const std::string& name);

// Zapamiętuje zakres ostatnio wydrukowanego testu – kreator filamentu pokazuje dokładnie te wartości do wyboru.
void note_calib(const Slic3r::Calib_Params& params);
void note_flow_pass(int pass, bool linear);

// Pierwsze uruchomienie (poziom Newbie): otwiera kreator raz.
void maybe_first_run_wizard(wxWindow* parent);

} // namespace Slic3r::GUI::Revo
