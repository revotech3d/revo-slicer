#include <boost/algorithm/string/predicate.hpp>
#include "slic3r/GUI/I18N.hpp"

#include "RevoKreator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/Http.hpp"

#include <nlohmann/json.hpp>

namespace Slic3r::GUI::Revo {

namespace {

double nozzle()
{
    const auto* n = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    return n && !n->values.empty() ? n->values.front() : 0.4;
}

wxStaticText* heading(wxWindow* p, const wxString& t)
{
    auto* s = new wxStaticText(p, wxID_ANY, t);
    s->SetFont(s->GetFont().Bold().Scaled(1.2f));
    return s;
}

std::string fmt(double v, int prec = 2)
{
    char b[32];
    snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

// ---------------------------------------------------------------- Newbie
class NewbieWizard : public wxDialog
{
public:
    explicit NewbieWizard(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("[r]evo – quick print setup"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    {
        auto* s  = new wxBoxSizer(wxVERTICAL);
        auto* pb = wxGetApp().preset_bundle;
        s->Add(heading(this, _L("Three choices and you are ready to print")), 0, wxALL, FromDIP(16));
        // [r]evo: najpierw maszyna (tylko Revon), potem materiał z profili Revotech
        s->Add(new wxStaticText(this, wxID_ANY, _L("Printer")), 0, wxLEFT | wxRIGHT, FromDIP(16));
        auto* prow = new wxBoxSizer(wxHORIZONTAL);
        m_printer = new wxChoice(this, wxID_ANY);
        for (const Preset& p : pb->printers)
            if (p.is_system && p.is_visible && boost::starts_with(p.name, "Revon")) {
                m_printers.push_back(p.name);
                m_printer->Append(from_u8(p.name));
                if (p.name == pb->printers.get_selected_preset_name())
                    m_printer->SetSelection(int(m_printers.size()) - 1);
            }
        if (m_printer->GetSelection() == wxNOT_FOUND && !m_printers.empty())
            m_printer->SetSelection(0);
        m_printer->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { select_printer(); fill_materials(); });
        prow->Add(m_printer, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        auto* con = new wxButton(this, wxID_ANY, _L("Connect to printer"));
        con->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_connect_wizard(this); });
        prow->Add(con, 0);
        s->Add(prow, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        s->Add(new wxStaticText(this, wxID_ANY, _L("Material")), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        m_material = new wxChoice(this, wxID_ANY);
        s->Add(m_material, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        select_printer();
        fill_materials();

        wxString q[] = {_L("Fast"), _L("Standard"), _L("Fine")};
        m_quality = new wxRadioBox(this, wxID_ANY, _L("Quality"), wxDefaultPosition, wxDefaultSize, 3, q, 3);
        m_quality->SetSelection(1);
        wxString st[] = {_L("Light"), _L("Standard"), _L("Strong")};
        m_strength = new wxRadioBox(this, wxID_ANY, _L("Strength"), wxDefaultPosition, wxDefaultSize, 3, st, 3);
        m_strength->SetSelection(1);
        wxString sp[] = {_L("Automatic"), _L("None")};
        m_support = new wxRadioBox(this, wxID_ANY, _L("Supports"), wxDefaultPosition, wxDefaultSize, 2, sp, 2);
        for (auto* r : {m_quality, m_strength, m_support})
            s->Add(r, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        auto* ok = new wxButton(this, wxID_OK, _L("Ready"));
        s->Add(ok, 0, wxALIGN_RIGHT | wxALL, FromDIP(16));
        ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply(); EndModal(wxID_OK); });
        SetSizerAndFit(s);
        CentreOnParent();
    }

private:
    void select_printer()
    {
        int i = m_printer->GetSelection();
        if (i < 0 || size_t(i) >= m_printers.size())
            return;
        if (wxGetApp().preset_bundle->printers.get_selected_preset_name() != m_printers[i])
            if (Tab* t = wxGetApp().get_tab(Preset::TYPE_PRINTER))
                t->select_preset(m_printers[i]);
    }

    void fill_materials()
    {
        auto* pb = wxGetApp().preset_bundle;
        m_material->Clear();
        m_names.clear();
        int pla = -1;
        for (const Preset& p : pb->filaments)
            if (p.is_visible && p.is_compatible && boost::starts_with(p.name, "Revo ")) {
                m_names.push_back(p.name);
                m_material->Append(from_u8(p.alias.empty() ? p.name : p.alias));
                if (pla < 0 && p.name.find("PLA") != std::string::npos)
                    pla = int(m_names.size()) - 1;
            }
        if (!m_names.empty())
            m_material->SetSelection(pla >= 0 ? pla : 0);
    }

    void apply()
    {
        select_printer();
        int sel = m_material->GetSelection();
        if (sel >= 0 && size_t(sel) < m_names.size())
            if (Tab* t = wxGetApp().get_tab(Preset::TYPE_FILAMENT))
                t->select_preset(m_names[sel]);
        Tab* tp = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tp)
            return;
        DynamicPrintConfig cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        const double       d   = nozzle();
        const double       lh[] = {0.7 * d, 0.5 * d, 0.3 * d};
        const int          inf[] = {10, 20, 40}, walls[] = {2, 3, 4};
        const int          q = m_quality->GetSelection(), st = m_strength->GetSelection();
        cfg.set_key_value("layer_height", new ConfigOptionFloat(std::round(lh[q] * 100) / 100));
        cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(inf[st]));
        cfg.set_key_value("wall_loops", new ConfigOptionInt(walls[st]));
        cfg.set_key_value("enable_support", new ConfigOptionBool(m_support->GetSelection() == 0));
        tp->load_config(cfg);
    }

    wxChoice*                m_material;
    wxChoice*                m_printer;
    std::vector<std::string> m_printers;
    wxRadioBox *             m_quality, *m_strength, *m_support;
    std::vector<std::string> m_names;
};

// ---------------------------------------------------------------- Monek
struct Step
{
    wxString   title, how;
    CalibKind  kind;
    wxString   unit;
    double     def, min, max, inc;
    int        digits;
};

class MonekWizard : public wxDialog
{
public:
    explicit MonekWizard(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("[r]evo – Monek profile wizard"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
    {
        const auto& cfg  = wxGetApp().preset_bundle->filaments.get_edited_preset().config;
        auto        get  = [&](const char* k, double d) {
            const ConfigOption* o = cfg.option(k);
            if (auto* f = dynamic_cast<const ConfigOptionFloats*>(o); f && !f->values.empty()) return f->values.front();
            if (auto* i = dynamic_cast<const ConfigOptionInts*>(o); i && !i->values.empty()) return double(i->values.front());
            return d;
        };
        m_steps = {
            {_L("Temperature"), _L("Print the temperature tower. Pick the floor with the best surface and bridges, enter its temperature."),
             CalibKind::Temperature, "°C", get("nozzle_temperature", 210), 150, 320, 5, 0},
            {_L("Max flowrate"), _L("Print the flow test. Find the height where the surface starts to break, read the flow from the scale."),
             CalibKind::MaxVolumetric, "mm³/s", get("filament_max_volumetric_speed", 20), 1, 80, 1, 1},
            {_L("Pressure advance"), _L("Print the pressure advance pattern. Pick the line with the sharpest corners, enter its value."),
             CalibKind::PressureAdvance, "", get("pressure_advance", 0.04), 0, 1, 0.005, 3},
            {_L("Flow ratio"), _L("Print the flow ratio test. Pick the smoothest top surface, enter the resulting flow ratio."),
             CalibKind::FlowRatio, "", get("filament_flow_ratio", 0.98), 0.7, 1.3, 0.01, 3},
            {_L("Retraction"), _L("Print the retraction tower. Pick the floor without strings, enter its retraction length."),
             CalibKind::Retraction, "mm", get("filament_retraction_length", 0.8), 0, 10, 0.1, 2},
        };
        auto* s = new wxBoxSizer(wxVERTICAL);
        m_title = heading(this, "");
        m_info  = new wxStaticText(this, wxID_ANY, "");
        m_info->Wrap(FromDIP(460));
        m_name  = new wxTextCtrl(this, wxID_ANY);
        m_value = new wxSpinCtrlDouble(this, wxID_ANY);
        m_print = new wxButton(this, wxID_ANY, _L("Prepare test print"));
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_back = new wxButton(this, wxID_ANY, _L("Back"));
        m_next = new wxButton(this, wxID_ANY, _L("Next"));
        row->Add(m_back);
        row->AddStretchSpacer();
        row->Add(m_next);
        s->Add(m_title, 0, wxALL, FromDIP(16));
        s->Add(m_info, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        s->Add(m_name, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        s->Add(m_print, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        s->Add(m_value, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        s->Add(row, 0, wxEXPAND | wxALL, FromDIP(16));
        m_name->SetMinSize(wxSize(FromDIP(460), -1));
        m_name->SetValue("Revo " + from_u8(wxGetApp().preset_bundle->filaments.get_edited_preset().alias));
        m_values.resize(m_steps.size());
        for (size_t i = 0; i < m_steps.size(); ++i)
            m_values[i] = m_steps[i].def;
        m_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_page >= 1 && m_page <= int(m_steps.size()))
                wxGetApp().mainframe->run_calibration(m_steps[m_page - 1].kind);
        });
        m_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { keep(); show(m_page - 1); });
        m_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            keep();
            if (m_page == int(m_steps.size()) + 1)
                return save();
            show(m_page + 1);
        });
        SetSizerAndFit(s);
        show(0);
        CentreOnParent();
    }

private:
    void keep()
    {
        if (m_page >= 1 && m_page <= int(m_steps.size()))
            m_values[m_page - 1] = m_value->GetValue();
    }

    void show(int page)
    {
        m_page = std::max(0, std::min(page, int(m_steps.size()) + 1));
        const bool step = m_page >= 1 && m_page <= int(m_steps.size());
        m_name->Show(m_page == 0);
        m_print->Show(step);
        m_value->Show(step);
        m_back->Enable(m_page > 0);
        if (m_page == 0) {
            m_title->SetLabel(_L("New filament profile"));
            m_info->SetLabel(_L("The wizard starts from the selected filament and walks you through five test prints. "
                                "Load the filament, then name the new profile."));
            m_next->SetLabel(_L("Start"));
        } else if (step) {
            const Step& st = m_steps[m_page - 1];
            m_title->SetLabel(wxString::Format(_L("Step %d of %d: %s"), m_page, int(m_steps.size()), st.title));
            m_info->SetLabel(st.how + (st.unit.empty() ? "" : " [" + st.unit + "]"));
            m_value->SetRange(st.min, st.max);
            m_value->SetIncrement(st.inc);
            m_value->SetDigits(st.digits);
            m_value->SetValue(m_values[m_page - 1]);
            m_next->SetLabel(_L("Next"));
        } else {
            m_title->SetLabel(_L("Save the profile"));
            m_info->SetLabel(wxString::Format(_L("Profile \"%s\" will be saved with the calibrated values."), m_name->GetValue()));
            m_next->SetLabel(_L("Save"));
        }
        m_info->Wrap(FromDIP(460));
        Layout();
        Fit();
    }

    void save()
    {
        const std::string name = into_u8(m_name->GetValue().Trim().Trim(false));
        if (name.empty())
            return show(0);
        Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
        if (!tab)
            return;
        DynamicPrintConfig cfg = wxGetApp().preset_bundle->filaments.get_edited_preset().config;
        auto set_all = [&cfg](const char* key, double v) {
            if (auto* f = cfg.option<ConfigOptionFloats>(key)) {
                for (double& x : f->values) x = v;
            } else if (auto* fn = cfg.option<ConfigOptionFloatsNullable>(key)) {
                for (double& x : fn->values) x = v;
            } else if (auto* i = cfg.option<ConfigOptionInts>(key)) {
                for (int& x : i->values) x = int(std::lround(v));
            } else if (auto* in = cfg.option<ConfigOptionIntsNullable>(key)) {
                for (int& x : in->values) x = int(std::lround(v));
            }
        };
        set_all("nozzle_temperature", m_values[0]);
        set_all("nozzle_temperature_initial_layer", m_values[0] + 5);
        set_all("filament_max_volumetric_speed", m_values[1]);
        set_all("pressure_advance", m_values[2]);
        if (auto* e = cfg.option<ConfigOptionBools>("enable_pressure_advance"))
            for (size_t i = 0; i < e->values.size(); ++i) e->values[i] = true;
        set_all("filament_flow_ratio", m_values[3]);
        set_all("filament_retraction_length", m_values[4]);
        tab->load_config(cfg);
        tab->save_preset(name);
        MessageDialog(this, wxString::Format(_L("Profile \"%s\" saved. Sometimes evolution. Sometimes [r]evolution."), from_u8(name)),
                      "[r]evo Slicer", wxOK | wxICON_INFORMATION).ShowModal();
        Destroy();
    }

    std::vector<Step>   m_steps;
    std::vector<double> m_values;
    int                 m_page = 0;
    wxStaticText *      m_title, *m_info;
    wxTextCtrl*         m_name;
    wxSpinCtrlDouble*   m_value;
    wxButton *          m_print, *m_back, *m_next;
};


// ---------------------------------------------------------------- Połączenie z drukarką
class ConnectWizard : public wxDialog
{
public:
    explicit ConnectWizard(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("[r]evo – connect to your Revon printer"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    {
        auto* s = new wxBoxSizer(wxVERTICAL);
        s->Add(heading(this, _L("Connect to your Revon printer")), 0, wxALL, FromDIP(16));
        auto* info = new wxStaticText(this, wxID_ANY,
            _L("Enter the printer address (revon.local or the IP shown on the printer screen in System) "
               "and the login and PIN of the machine manager. The slicer gets the access key from the printer by itself."));
        info->Wrap(FromDIP(440));
        s->Add(info, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        m_host  = new wxTextCtrl(this, wxID_ANY, "revon.local");
        m_login = new wxTextCtrl(this, wxID_ANY);
        m_pin   = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_login->SetHint(_L("Manager login"));
        m_pin->SetHint("PIN");
        for (auto* c : {m_host, m_login, m_pin})
            s->Add(c, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        m_err = new wxStaticText(this, wxID_ANY, "");
        m_err->SetForegroundColour(wxColour(255, 77, 82));
        s->Add(m_err, 0, wxLEFT | wxRIGHT, FromDIP(16));
        auto* ok = new wxButton(this, wxID_OK, _L("Connect"));
        s->Add(ok, 0, wxALIGN_RIGHT | wxALL, FromDIP(16));
        ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { connect(); });
        m_host->SetMinSize(wxSize(FromDIP(440), -1));
        SetSizerAndFit(s);
        CentreOnParent();
    }

private:
    void fail(const wxString& msg)
    {
        m_err->SetLabel(msg);
        m_err->Wrap(FromDIP(440));
        Layout();
        Fit();
    }

    void connect()
    {
        wxBusyCursor      busy;
        std::string       host = into_u8(m_host->GetValue().Trim().Trim(false));
        if (host.rfind("http", 0) != 0)
            host = "http://" + host;
        while (!host.empty() && host.back() == '/')
            host.pop_back();
        std::string cookie, key, err;
        unsigned    st = 0;
        nlohmann::json body = {{"login", into_u8(m_login->GetValue())}, {"pin", into_u8(m_pin->GetValue())}};
        Http::post(host + "/revo/sesja/operator")
            .header("Content-Type", "application/json")
            .timeout_connect(5)
            .timeout_max(15)
            .set_post_body(body.dump())
            .on_header_callback([&cookie](std::string h) {
                // nagłówek „set-cookie: revo_sesja=…; Path=/…”
                std::string low = h;
                std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                auto p = low.find("revo_sesja=");
                if (p != std::string::npos) {
                    auto e = h.find(';', p);
                    cookie = h.substr(p, e == std::string::npos ? std::string::npos : e - p);
                }
            })
            .on_complete([&](std::string, unsigned s) { st = s; })
            .on_error([&](std::string b, std::string e, unsigned s) {
                st = s;
                try { err = nlohmann::json::parse(b).value("message", e); } catch (...) { err = e; }
            })
            .perform_sync();
        if (st == 0)
            return fail(_L("The printer does not respond. Check the address and that the computer is in the same network.") +
                        (err.empty() ? wxString() : "\n(" + from_u8(err) + ")"));
        if (st != 200 || cookie.empty())
            return fail(from_u8(err.empty() ? _u8L("Wrong login or PIN.") : err));
        Http::get(host + "/revo/klucz-api")
            .header("Cookie", cookie)
            .timeout_connect(5)
            .timeout_max(15)
            .on_complete([&](std::string b, unsigned) {
                try { key = nlohmann::json::parse(b).value("klucz", ""); } catch (...) {}
            })
            .on_error([&](std::string b, std::string e, unsigned) {
                try { err = nlohmann::json::parse(b).value("message", e); } catch (...) { err = e; }
            })
            .perform_sync();
        if (key.empty())
            return fail(from_u8(err.empty() ? _u8L("Only the machine manager can connect the slicer.") : err));

        Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
        if (!tab)
            return;
        auto&              pr  = wxGetApp().preset_bundle->printers;
        DynamicPrintConfig cfg = pr.get_edited_preset().config;
        cfg.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(htMoonraker));
        cfg.set_key_value("print_host", new ConfigOptionString(host.substr(host.find("//") + 2)));
        cfg.set_key_value("printhost_apikey", new ConfigOptionString(key));
        std::string base = pr.get_edited_preset().is_system ? pr.get_edited_preset().name : pr.get_selected_preset_parent() ? pr.get_selected_preset_parent()->name : pr.get_edited_preset().name;
        tab->load_config(cfg);
        tab->save_preset(base + " @ " + host.substr(host.find("//") + 2));
        EndModal(wxID_OK);
        MessageDialog(GetParent(), _L("Connected. Send prints to the printer with the Print button."), "[r]evo Slicer", wxOK | wxICON_INFORMATION).ShowModal();
    }

    wxTextCtrl *  m_host, *m_login, *m_pin;
    wxStaticText* m_err;
};

} // namespace

void show_newbie_wizard(wxWindow* parent)
{
    NewbieWizard dlg(parent);
    dlg.ShowModal();
}

void show_monek_wizard(wxWindow* parent)
{
    // niemodalny – zostaje otwarty, gdy drukujesz testy
    (new MonekWizard(parent))->Show();
}

void show_connect_wizard(wxWindow* parent)
{
    ConnectWizard dlg(parent);
    dlg.ShowModal();
}

void maybe_first_run_wizard(wxWindow* parent)
{
    AppConfig* c = wxGetApp().app_config;
    if (!c || c->get("revo_kreator_pokazany") == "1")
        return;
    // pierwsze uruchomienie: maszyny Revon instaluje config_wizard_startup() – kreator dopiero po nim (wywoła go ponownie)
    if (wxGetApp().preset_bundle->printers.only_default_printers())
        return;
    c->set("revo_kreator_pokazany", "1");
    show_newbie_wizard(parent);
}

} // namespace Slic3r::GUI::Revo
