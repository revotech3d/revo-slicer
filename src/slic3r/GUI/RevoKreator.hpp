#pragma once
// [r]evo Slicer – kreatory:
//  - Newbie: jak najmniej pytań (wzór: prostota Stratasysa) – materiał, jakość, wytrzymałość, podpory i gotowe.
//  - Monek: kalibracja nowego filamentu krok po kroku (temperatura, przepływ, PA, współczynnik, retrakcja) i zapis profilu.

#include <string>

class wxWindow;

namespace Slic3r::GUI::Revo {

void show_newbie_wizard(wxWindow* parent);
void show_monek_wizard(wxWindow* parent);
// Połączenie z drukarką Revon: adres + login i PIN kierownika → klucz API z maszyny → zapisana drukarka z wysyłaniem.
void show_connect_wizard(wxWindow* parent);

// „My printers” na ekranie startowym: dodane maszyny (profil z adresem) ze stanem z sieci; klik = aktywna drukarka.
void send_my_printers(bool refresh = true);
void select_my_printer(const std::string& name);

// Pierwsze uruchomienie (poziom Newbie): otwiera kreator raz.
void maybe_first_run_wizard(wxWindow* parent);

} // namespace Slic3r::GUI::Revo
