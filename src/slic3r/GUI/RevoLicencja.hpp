#pragma once
// [r]evo Slicer – logowanie kontem Revotech i licencja (numer licencji maszyny Revon).
// Token od serwera revo3d.pl jest podpisany kluczem Revotech (Ed25519) i ważny 14 dni bez połączenia.

#include <string>

class wxWindow;

namespace Slic3r { class AppConfig; }

namespace Slic3r::GUI::Revo {

// Wywoływane przy starcie, przed wczytaniem profili. false = zakończ program.
bool ensure_licence(Slic3r::AppConfig& cfg, wxWindow* parent);

// Konto serwisowe Revotech: odblokowane tworzenie własnych drukarek.
bool is_service();

// Login zalogowanego konta (do okna „O programie”).
std::string login();

} // namespace Slic3r::GUI::Revo
