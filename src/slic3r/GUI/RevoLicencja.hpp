#pragma once
// [r]evo Slicer – tożsamość maszyn Revon i logowanie serwisu Revotech.
// Slicer działa bez logowania, ale wysyła wydruki tylko do maszyn Revon: maszyna pokazuje licencję podpisaną
// kluczem Revotech (Ed25519). Logowanie (token ważny 14 dni bez połączenia) jest tylko dla serwisu Revotech.

#include <string>

class wxWindow;

namespace Slic3r { class AppConfig; }

namespace Slic3r::GUI::Revo {

// Logowanie serwisu Revotech (Pomoc → Logowanie serwisu). true = zalogowany.
bool ensure_licence(Slic3r::AppConfig& cfg, wxWindow* parent);

// Przy starcie: wczytaj zapisany token serwisu (bez okna logowania).
void load_saved(Slic3r::AppConfig& cfg);

// Odpowiedź GET /revo/sesja/tozsamosc: true gdy to maszyna Revon (licencja podpisana przez Revotech).
bool is_revon_machine(const std::string& body);

// Konto serwisowe Revotech: odblokowane tworzenie własnych drukarek.
bool is_service();

// Login zalogowanego konta (do okna „O programie”).
std::string login();

} // namespace Slic3r::GUI::Revo
