#pragma once
// [r]evo Slicer – poziomy Newbie / Pro / Monek. Monek pokazuje tylko ustawienia,
// które Revotech zaleca zmieniać (lista w resources/revo/monek.txt). Widoczność jak w trybie Ekspert, zawężona do listy.

#include <string>

namespace Slic3r::GUI::Revo::Monek {

// Wartość dla GUI_App::save_mode() oznaczająca tryb Monek (poza zakresem ConfigOptionMode).
constexpr int MODE = 100;
// Pozycja trybu Monek w przełączniku trybów.
constexpr int SELECTION = 2;

bool active();
// Czy opcja (klucz albo „klucz#indeks”) jest widoczna – zawsze true poza trybem Monek.
bool allows(const std::string& opt_id);

} // namespace Slic3r::GUI::Revo::Monek
