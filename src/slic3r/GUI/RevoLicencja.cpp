#include "RevoLicencja.hpp"

#include "slic3r/GUI/I18N.hpp"

#include <ctime>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r::GUI::Revo {

namespace {

const char* SERVER = "https://revo3d.pl/api/slicer/";
// Klucz publiczny Revotech (Ed25519, SPKI DER base64) – revo3d.pl/api/slicer/klucz
const char* PUBLIC_KEY_B64 = "MCowBQYDK2VwAyEA+GdqsvnNhxkv3xLlRPhUwMrWZ4NziFFDrO1K9VK0mT4=";

struct Token
{
    std::string login, rola, licencja;
    long long   wygasa = 0;
};

Token g_token;

std::vector<unsigned char> b64decode(const std::string& in)
{
    std::vector<unsigned char> out(3 * in.size() / 4 + 3);
    int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(in.data()), int(in.size()));
    if (n < 0)
        return {};
    size_t pad = 0;
    for (auto it = in.rbegin(); it != in.rend() && *it == '='; ++it)
        ++pad;
    out.resize(size_t(n) - pad);
    return out;
}

// Podpis Ed25519 kluczem Revotech; treść tokenu tylko gdy podpis się zgadza.
bool verify(const std::string& payload, const std::string& sig_b64, Token& out)
{
    auto der = b64decode(PUBLIC_KEY_B64), sig = b64decode(sig_b64);
    if (der.size() != 44 || sig.size() != 64)
        return false;
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, der.data() + 12, 32);
    if (!key)
        return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
              EVP_DigestVerify(ctx, sig.data(), sig.size(), reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!ok)
        return false;
    try {
        auto j = nlohmann::json::parse(payload);
        if (j.value("typ", "") != "slicer")
            return false;
        out.login    = j.value("login", "");
        out.rola     = j.value("rola", "klient");
        out.licencja = j.value("licencja", "brak");
        out.wygasa   = j.value("wygasa", 0LL);
    } catch (...) {
        return false;
    }
    return true;
}

struct Reply
{
    unsigned       status = 0;
    nlohmann::json body;
};

Reply post(const std::string& path, const nlohmann::json& body)
{
    Reply r;
    auto  done = [&r](std::string b, unsigned st) {
        r.status = st;
        try { r.body = nlohmann::json::parse(b); } catch (...) { r.body = nlohmann::json::object(); }
    };
    Http::post(SERVER + path)
        .header("Content-Type", "application/json")
        .timeout_connect(5)
        .timeout_max(20)
        .set_post_body(body.dump())
        .on_complete([&](std::string b, unsigned st) { done(std::move(b), st); })
        .on_error([&](std::string b, std::string, unsigned st) { done(std::move(b), st); })
        .perform_sync();
    return r;
}

std::string message(const Reply& r)
{
    if (r.status == 0)
        return _u8L("No connection to revo3d.pl. Check the internet connection.");
    return r.body.value("message", std::string("HTTP ") + std::to_string(r.status));
}

// zapisany token → g_token; true gdy ważny podpis
bool load_token(AppConfig& cfg)
{
    return verify(cfg.get("revo_token_payload"), cfg.get("revo_token_sig"), g_token);
}

bool store_token(AppConfig& cfg, const nlohmann::json& tok)
{
    if (!tok.is_object())
        return false;
    Token t;
    if (!verify(tok.value("payload", ""), tok.value("signature", ""), t))
        return false;
    cfg.set("revo_token_payload", tok.value("payload", ""));
    cfg.set("revo_token_sig", tok.value("signature", ""));
    g_token = t;
    return true;
}

bool usable(const Token& t)
{
    return t.licencja == "aktywna" && t.wygasa > (long long) std::time(nullptr);
}

// Jedno okno na wszystkie kroki: logowanie → (nowe hasło) → (czekanie na licencję maszyny).
// Numer licencji wpisuje się tylko na ekranie drukarki; slicer działa, gdy aktywna jest maszyna przypisana do konta.
class LoginDialog : public wxDialog
{
public:
    enum class Step { Login, NewPassword, Licence };

    LoginDialog(wxWindow* parent, AppConfig& cfg)
        : wxDialog(parent, wxID_ANY, "[r]evo Slicer", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_cfg(cfg)
    {
        auto* s = new wxBoxSizer(wxVERTICAL);
        m_title = new wxStaticText(this, wxID_ANY, "");
        m_title->SetFont(m_title->GetFont().Bold().Scaled(1.3f));
        m_info  = new wxStaticText(this, wxID_ANY, "");
        m_a     = new wxTextCtrl(this, wxID_ANY);
        m_b     = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_err   = new wxStaticText(this, wxID_ANY, "");
        m_err->SetForegroundColour(wxColour(255, 77, 82));
        m_ok    = new wxButton(this, wxID_OK, "");
        auto* cancel = new wxButton(this, wxID_CANCEL, _L("Quit"));
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddStretchSpacer();
        row->Add(cancel, 0, wxRIGHT, FromDIP(8));
        row->Add(m_ok);
        s->Add(m_title, 0, wxALL, FromDIP(16));
        s->Add(m_info, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        s->Add(m_a, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        s->Add(m_b, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        s->Add(m_err, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        s->Add(row, 0, wxEXPAND | wxALL, FromDIP(16));
        m_a->SetMinSize(wxSize(FromDIP(380), -1));
        SetSizerAndFit(s);
        m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { submit(); });
        set_step(Step::Login);
        CentreOnScreen();
    }

    void set_step(Step st, const wxString& err = "")
    {
        m_step = st;
        m_a->Clear();
        m_b->Clear();
        m_a->Show(st != Step::Licence);
        m_b->Show(st != Step::Licence);
        if (st == Step::Login) {
            m_title->SetLabel(_L("Sign in to [r]evo Slicer"));
            m_info->SetLabel(_L("Use the login and password you received from Revotech."));
            m_a->SetHint(_L("Login"));
            m_b->SetHint(_L("Password"));
            m_ok->SetLabel(_L("Sign in"));
        } else if (st == Step::NewPassword) {
            m_title->SetLabel(_L("Set your own password"));
            m_info->SetLabel(_L("The password from Revotech is temporary. Enter a new one (at least 10 characters) twice."));
            m_a->SetHint(_L("New password"));
            m_a->SetWindowStyleFlag(m_a->GetWindowStyleFlag() | wxTE_PASSWORD);
            m_b->SetHint(_L("Repeat the new password"));
            m_ok->SetLabel(_L("Save"));
        } else {
            m_title->SetLabel(_L("Activate your printer"));
            m_info->SetLabel(_L("[r]evo Slicer starts when your Revon printer is activated.\nEnter the licence number on the printer screen: Menu → Więcej → Licencja,\nthen click Check again."));
            m_ok->SetLabel(_L("Check again"));
        }
        m_err->SetLabel(err);
        Layout();
        Fit();
        if (m_a->IsShown())
            m_a->SetFocus();
    }

private:
    void submit()
    {
        wxBusyCursor busy;
        const std::string a = m_a->GetValue().ToStdString(), b = m_b->GetValue().ToStdString();
        if (m_step == Step::Login) {
            Reply r = post("login", {{"login", a}, {"haslo", b}});
            if (r.status != 200)
                return set_step(Step::Login, from_u8(message(r)));
            m_cfg.set("revo_login", r.body.value("login", a));
            m_cfg.set("revo_sesja", r.body.value("sesja", ""));
            if (!store_token(m_cfg, r.body["token"]))
                return set_step(Step::Login, _L("The server response is not signed by Revotech."));
            if (r.body.value("zmien_haslo", false))
                return set_step(Step::NewPassword);
            return next();
        }
        if (m_step == Step::NewPassword) {
            if (a != b)
                return set_step(Step::NewPassword, _L("The passwords are different."));
            Reply r = post("haslo", {{"login", m_cfg.get("revo_login")}, {"sesja", m_cfg.get("revo_sesja")}, {"nowe", a}});
            if (r.status != 200)
                return set_step(Step::NewPassword, from_u8(message(r)));
            store_token(m_cfg, r.body["token"]);
            return next();
        }
        Reply r = post("odnow", {{"login", m_cfg.get("revo_login")}, {"sesja", m_cfg.get("revo_sesja")}});
        if (r.status == 401)
            return set_step(Step::Login, from_u8(message(r)));
        if (r.status != 200)
            return set_step(Step::Licence, from_u8(message(r)));
        store_token(m_cfg, r.body["token"]);
        next();
    }

    void next()
    {
        if (g_token.licencja == "zablokowana")
            return set_step(Step::Login, _L("The licence is blocked. Contact Revotech: biuro@revo3d.pl"));
        if (g_token.licencja != "aktywna")
            return set_step(Step::Licence, m_step == Step::Licence ? _L("The printer is not activated yet.") : wxString());
        m_cfg.save();
        EndModal(wxID_OK);
    }

    AppConfig&    m_cfg;
    Step          m_step = Step::Login;
    wxStaticText *m_title, *m_info, *m_err;
    wxTextCtrl *  m_a, *m_b;
    wxButton*     m_ok;
};

} // namespace

bool ensure_licence(AppConfig& cfg, wxWindow* parent)
{
    load_token(cfg);
    // z siecią: odnów token (blokada konta lub licencji działa od razu); bez sieci: zapisany token do 14 dni
    if (!cfg.get("revo_sesja").empty()) {
        Reply r = post("odnow", {{"login", cfg.get("revo_login")}, {"sesja", cfg.get("revo_sesja")}});
        if (r.status == 200) {
            store_token(cfg, r.body["token"]);
            cfg.save();
        } else if (r.status == 401) {
            cfg.set("revo_sesja", "");
            g_token = Token();
        }
    }
    if (usable(g_token))
        return true;
    LoginDialog dlg(parent, cfg);
    return dlg.ShowModal() == wxID_OK && usable(g_token);
}

bool is_service() { return usable(g_token) && g_token.rola == "serwis"; }

std::string login() { return g_token.login; }

} // namespace Slic3r::GUI::Revo
