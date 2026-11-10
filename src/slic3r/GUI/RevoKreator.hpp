#pragma once
// [r]evo Slicer – kreatory:
//  - Newbie: jak najmniej pytań (wzór: prostota Stratasysa) – materiał, jakość, wytrzymałość, podpory i gotowe.
//  - Monek: kalibracja nowego filamentu krok po kroku (temperatura, przepływ, PA, współczynnik, retrakcja) i zapis profilu.

class wxWindow;

namespace Slic3r::GUI::Revo {

void show_newbie_wizard(wxWindow* parent);
void show_monek_wizard(wxWindow* parent);

// Pierwsze uruchomienie (poziom Newbie): otwiera kreator raz.
void maybe_first_run_wizard(wxWindow* parent);

} // namespace Slic3r::GUI::Revo
