#include <boost/algorithm/string/predicate.hpp>
#include "slic3r/GUI/I18N.hpp"

#include "RevoKreator.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/statbmp.h>
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
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/Http.hpp"

#include <nlohmann/json.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Utils.hpp"

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
        con->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            show_connect_wizard(this);
            sync_printer(); // połączenie mogło przełączyć profil na podłączoną maszynę
        });
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
    void sync_printer()
    {
        const std::string cur = wxGetApp().preset_bundle->printers.get_selected_preset().name;
        for (size_t i = 0; i < m_printers.size(); ++i)
            if (boost::starts_with(cur, m_printers[i]) && int(i) != m_printer->GetSelection()) {
                m_printer->SetSelection(int(i));
                fill_materials();
            }
    }

    void select_printer()
    {
        int i = m_printer->GetSelection();
        if (i < 0 || size_t(i) >= m_printers.size())
            return;
        // profil zapisany po połączeniu („<system> @ host”) zostaje – nie wracamy do systemowego
        if (!boost::starts_with(wxGetApp().preset_bundle->printers.get_selected_preset_name(), m_printers[i]))
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


// ---------------------------------------------------------------- Wyszukiwanie drukarek w sieci
struct FoundPrinter
{
    std::string ip, id, model, state;
    double      nozzle = 0;
};

// Adres IPv4 tego komputera w sieci lokalnej (gniazdo UDP bez wysyłania pakietów), pusty gdy brak.
std::string local_ipv4()
{
    try {
        boost::asio::io_context      io;
        boost::asio::ip::udp::socket so(io);
        so.connect(boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("192.168.255.255"), 9));
        return so.local_endpoint().address().to_string();
    } catch (...) {
        return {};
    }
}

// Pyta /revo/sesja/odkryj (agent Revo, bez logowania) pod revon.local i pod każdym adresem sieci /24 tego komputera.
std::vector<FoundPrinter> discover_printers()
{
    std::vector<std::string> hosts{"revon.local"};
    const std::string        me = local_ipv4();
    const auto               dot = me.rfind('.');
    if (dot != std::string::npos && me.rfind("127.", 0) != 0)
        for (int i = 1; i < 255; ++i)
            if (me.substr(dot + 1) != std::to_string(i))
                hosts.push_back(me.substr(0, dot + 1) + std::to_string(i));

    std::vector<FoundPrinter> out;
    std::mutex                mtx;
    std::atomic<size_t>       next{0};
    auto                      worker = [&] {
        for (size_t i; (i = next++) < hosts.size();) {
            const std::string h = hosts[i];
            Http::get("http://" + h + "/revo/sesja/odkryj")
                .timeout_connect(1)
                .timeout_max(3)
                .on_complete([&](std::string b, unsigned st) {
                    try {
                        const auto j = nlohmann::json::parse(b);
                        if (st != 200 || j.value("revo", 0) != 1)
                            return;
                        FoundPrinter f;
                        f.ip     = h;
                        f.id     = j.value("machine_id", "");
                        f.model  = j.value("model", "");
                        f.state  = j.value("state", "");
                        f.nozzle = j.contains("dysza") && j["dysza"].is_number() ? j["dysza"].get<double>() : 0;
                        std::lock_guard<std::mutex> l(mtx);
                        // revon.local i jego IP to ta sama maszyna – zostaje jeden wpis (z IP)
                        auto it = std::find_if(out.begin(), out.end(), [&](const FoundPrinter& o) { return o.id == f.id; });
                        if (it == out.end())
                            out.push_back(f);
                        else if (it->ip == "revon.local")
                            it->ip = f.ip;
                    } catch (...) {}
                })
                .perform_sync();
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < 48; ++t)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
    std::sort(out.begin(), out.end(), [](const FoundPrinter& a, const FoundPrinter& b) { return a.id < b.id; });
    return out;
}

wxString model_name(const std::string& m)
{
    if (m == "belt") return "Revon Belt 350";
    if (m == "600x600x400") return "Revon 600 Z400";
    if (m == "600x600x600") return "Revon 600";
    if (m == "1200x600x500") return "Revon 1200";
    return "Revon";
}

wxString state_name(const std::string& s)
{
    if (s == "gotowa") return _L("Ready");
    if (s == "drukowanie") return _L("Printing");
    if (s == "nagrzewanie") return _L("Heating");
    if (s == "pauza") return _L("Paused");
    if (s == "blad") return _L("Error");
    return _L("Starting");
}

wxBitmap model_thumb(wxWindow* w, const std::string& m)
{
    const std::string f = m == "belt" ? "Revon Belt 350x350x300" : "Revon " + (m.empty() ? std::string("600x600x600") : m);
    wxImage img;
    const auto path = boost::filesystem::path(resources_dir()) / "profiles" / "Revotech" / (f + "_cover.png");
    if (!img.LoadFile(from_u8(path.string()), wxBITMAP_TYPE_PNG))
        return wxBitmap();
    const int px = w->FromDIP(84);
    return wxBitmap(img.Rescale(px, px * img.GetHeight() / std::max(1, img.GetWidth()), wxIMAGE_QUALITY_HIGH));
}

wxString card_text(const FoundPrinter& f)
{
    // TRN %1% model, %2% serial number, %3% state, %4% IP address, %5% nozzle diameter
    return format_wxstr(_L("%1%\nSerial number: %2%\nState: %3%\nIP: %4%   Nozzle: %5% mm"), model_name(f.model), f.id, state_name(f.state), f.ip,
                        f.nozzle > 0 ? wxString::Format("%.1f", f.nozzle) : wxString("?"));
}

// Lista znalezionych maszyn: miniatura, numer, stan, IP – klik „Select” wybiera.
class PickPrinterDialog : public wxDialog
{
public:
    PickPrinterDialog(wxWindow* parent, const std::vector<FoundPrinter>& list)
        : wxDialog(parent, wxID_ANY, _L("[r]evo – printers in your network"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    {
        auto* s = new wxBoxSizer(wxVERTICAL);
        s->Add(heading(this, _L("Choose your printer")), 0, wxALL, FromDIP(16));
        auto* sw = new wxScrolledWindow(this, wxID_ANY);
        auto* ls = new wxBoxSizer(wxVERTICAL);
        for (size_t i = 0; i < list.size(); ++i) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticBitmap(sw, wxID_ANY, model_thumb(this, list[i].model)), 0, wxRIGHT, FromDIP(12));
            row->Add(new wxStaticText(sw, wxID_ANY, card_text(list[i])), 1, wxALIGN_CENTER_VERTICAL);
            auto* b = new wxButton(sw, wxID_ANY, _L("Select"));
            b->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { m_sel = int(i); EndModal(wxID_OK); });
            row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
            ls->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
        }
        sw->SetSizer(ls);
        sw->SetScrollRate(0, FromDIP(10));
        sw->SetMinSize(wxSize(FromDIP(460), std::min<int>(FromDIP(110) * int(list.size()), FromDIP(440))));
        s->Add(sw, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        s->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0, wxALIGN_RIGHT | wxALL, FromDIP(16));
        SetSizerAndFit(s);
        CentreOnParent();
    }
    int selection() const { return m_sel; }

private:
    int m_sel = -1;
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
               "and click Connect. The login and PIN of the machine manager are needed only if the printer requires them."));
        info->Wrap(FromDIP(440));
        s->Add(info, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        // [r]evo: drukarka znaleziona w sieci (miniatura, numer, stan, IP) i ponowne szukanie
        auto* card = new wxBoxSizer(wxHORIZONTAL);
        m_thumb    = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
        m_found    = new wxStaticText(this, wxID_ANY, _L("Searching for printers in your network…"));
        auto* again = new wxButton(this, wxID_ANY, _L("Search again"));
        again->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { search(); });
        card->Add(m_thumb, 0, wxRIGHT, FromDIP(12));
        card->Add(m_found, 1, wxALIGN_CENTER_VERTICAL);
        card->Add(again, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        s->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        m_host  = new wxTextCtrl(this, wxID_ANY, "revon.local");
        m_login = new wxTextCtrl(this, wxID_ANY);
        m_pin   = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_login->SetHint(_L("Manager login (optional)"));
        m_pin->SetHint(_L("PIN (optional)"));
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
        CallAfter([this] { search(); });
    }

private:
    void search()
    {
        m_found->SetLabel(_L("Searching for printers in your network…"));
        m_thumb->SetBitmap(wxNullBitmap);
        Layout();
        Update();
        std::vector<FoundPrinter> list;
        {
            wxBusyCursor busy;
            list = discover_printers();
        }
        int sel = list.size() == 1 ? 0 : -1;
        if (list.size() > 1) {
            PickPrinterDialog dlg(this, list);
            if (dlg.ShowModal() == wxID_OK)
                sel = dlg.selection();
        }
        if (list.empty())
            m_found->SetLabel(_L("No printer found. Check that the printer is on and in the same network, or enter its address."));
        else if (sel < 0)
            m_found->SetLabel(format_wxstr(_L_PLURAL("Found %1% printer – click Search again to choose.", "Found %1% printers – click Search again to choose.", list.size()), list.size()));
        else {
            m_host->SetValue(from_u8(list[sel].ip));
            m_thumb->SetBitmap(model_thumb(this, list[sel].model));
            m_found->SetLabel(card_text(list[sel]));
        }
        m_found->Wrap(FromDIP(330));
        Layout();
        Fit();
    }

    void fail(const wxString& msg)
    {
        m_err->SetLabel(msg);
        m_err->Wrap(FromDIP(440));
        Layout();
        Fit();
    }

    // Nazwa profilu „Revon … <dysza> nozzle” dla maszyny pod adresem host (Moonraker przez klucz API), pusta gdy się nie da.
    static std::string printer_profile(const std::string& host, const std::string& key)
    {
        std::string out;
        Http::get(host + "/printer/objects/query?toolhead=axis_maximum&save_variables")
            .header("X-Api-Key", key)
            .timeout_connect(5)
            .timeout_max(10)
            .on_complete([&out](std::string b, unsigned) {
                try {
                    const auto st  = nlohmann::json::parse(b).at("result").at("status");
                    const auto ax  = st.at("toolhead").at("axis_maximum");
                    const double x = ax.at(0).get<double>(), z = ax.at(2).get<double>();
                    std::string m;
                    if (x < 500)
                        m = "Revon Belt 350x350x300";
                    else if (x > 900)
                        m = "Revon 1200x600x500";
                    else
                        m = z > 500 ? "Revon 600x600x600" : "Revon 600x600x400";
                    double d = 0.4;
                    const auto& v = st.at("save_variables").at("variables");
                    if (v.contains("revo_nozzle_diameter") && v["revo_nozzle_diameter"].is_number())
                        d = v["revo_nozzle_diameter"].get<double>();
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%.1f", d < 0.3 ? 0.4 : d); // profile od 0.4 mm
                    out = m + " " + buf + " nozzle";
                } catch (...) {}
            })
            .perform_sync();
        return out;
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
        if (m_login->GetValue().Trim().IsEmpty()) {
            // [r]evo: bez logowania (jak Orca) – drukarka oddaje klucz, chyba że kierownik włączył wymóg logowania
            bool need_login = false;
            Http::get(host + "/revo/sesja/klucz-slicer")
                .timeout_connect(5)
                .timeout_max(15)
                .on_complete([&](std::string b, unsigned s) {
                    st = s;
                    try { key = nlohmann::json::parse(b).value("klucz", ""); } catch (...) {}
                })
                .on_error([&](std::string b, std::string e, unsigned s) {
                    st = s;
                    try {
                        const auto j = nlohmann::json::parse(b);
                        need_login   = j.value("logowanie", false);
                        err          = j.value("message", e);
                    } catch (...) { err = e; }
                })
                .perform_sync();
            if (st == 0)
                return fail(_L("The printer does not respond. Check the address and that the computer is in the same network.") +
                            (err.empty() ? wxString() : "\n(" + from_u8(err) + ")"));
            if (need_login) {
                m_login->SetFocus();
                return fail(_L("This printer requires the manager login. Enter the login and PIN."));
            }
            if (key.empty())
                return fail(from_u8(err.empty() ? _u8L("The printer did not give the access key. Update Revo OS on the printer.") : err));
        } else {
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
        }

        Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
        if (!tab)
            return;
        auto& pr = wxGetApp().preset_bundle->printers;
        // [r]evo: profil drukarki dobrany do podłączonej maszyny (rozmiar osi + założona dysza z Revo OS)
        const std::string model = printer_profile(host, key);
        if (!model.empty() && model != pr.get_selected_preset_name()) {
            const Preset* p = pr.find_preset(model, false);
            if (p && p->is_visible)
                tab->select_preset(model);
        }
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

    wxTextCtrl *    m_host, *m_login, *m_pin;
    wxStaticText*   m_err;
    wxStaticText*   m_found;
    wxStaticBitmap* m_thumb;
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
