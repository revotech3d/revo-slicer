#include <boost/algorithm/string/predicate.hpp>
#include "slic3r/GUI/I18N.hpp"

#include "RevoKreator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <wx/app.h>
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
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/hyperlink.h>
#include <wx/panel.h>
#include <wx/timer.h>

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
    std::string ip, id, model, state, os;
    double      nozzle   = 0;
    int         progress = 0;
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
// Działa w wątkach roboczych: on_found dostaje każdą maszynę od razu (raz na numer seryjny), stop przerywa szukanie.
void discover_printers(const std::function<void(const FoundPrinter&)>& on_found, const std::shared_ptr<std::atomic<bool>>& stop)
{
    std::vector<std::string> hosts{"revon.local"};
    const std::string        me = local_ipv4();
    const auto               dot = me.rfind('.');
    if (dot != std::string::npos && me.rfind("127.", 0) != 0)
        for (int i = 1; i < 255; ++i)
            if (me.substr(dot + 1) != std::to_string(i))
                hosts.push_back(me.substr(0, dot + 1) + std::to_string(i));

    std::vector<std::string> seen;
    std::mutex               mtx;
    std::atomic<size_t>      next{0};
    auto                     worker = [&] {
        for (size_t i; !*stop && (i = next++) < hosts.size();) {
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
                        f.nozzle   = j.contains("dysza") && j["dysza"].is_number() ? j["dysza"].get<double>() : 0;
                        f.progress = j.contains("postep") && j["postep"].is_number() ? j["postep"].get<int>() : 0;
                        f.os       = j.value("revo_os", "");
                        {
                            std::lock_guard<std::mutex> l(mtx);
                            if (std::find(seen.begin(), seen.end(), f.id) != seen.end())
                                return; // revon.local i jego IP to ta sama maszyna
                            seen.push_back(f.id);
                        }
                        on_found(f);
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

wxString model_dims(const std::string& m)
{
    if (m == "belt") return _L("350 × 350 × 300 mm · belt");
    if (m == "600x600x400") return "600 × 600 × 400 mm";
    if (m == "600x600x600") return "600 × 600 × 600 mm";
    if (m == "1200x600x500") return "1200 × 600 × 500 mm";
    return "";
}

wxColour state_colour(const std::string& s)
{
    if (s == "gotowa") return wxColour(62, 214, 99);
    if (s == "drukowanie" || s == "nagrzewanie" || s == "pauza") return wxColour(240, 160, 75);
    if (s == "blad") return wxColour(255, 77, 82);
    return wxColour(154, 160, 168);
}

constexpr double kPi = 3.14159265358979323846;

// Kolory ekranu wyszukiwania (spójne z Revo OS)
const wxColour kBg(22, 24, 27), kCard(32, 36, 40), kLine(44, 50, 56), kText(232, 234, 237), kMuted(154, 160, 168), kGreen(62, 214, 99);

// ---------------------------------------------------------------- Radar: obracająca się wiązka, znalezione maszyny jako punkty
class RadarPanel : public wxPanel
{
public:
    explicit RadarPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY), m_timer(this)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(230, 230)));
        Bind(wxEVT_PAINT, &RadarPanel::on_paint, this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { m_angle = std::fmod(m_angle + 3.0, 360.0); Refresh(false); });
        m_timer.Start(33);
    }
    ~RadarPanel() override { m_timer.Stop(); }

    // Punkt dla maszyny: kąt i odległość wyliczone z numeru seryjnego (zawsze w tym samym miejscu radaru)
    void add_blip(const std::string& id)
    {
        const size_t h = std::hash<std::string>{}(id);
        m_blips.push_back({double(h % 360), 0.35 + 0.5 * double((h / 360) % 100) / 100.0});
    }
    void clear_blips() { m_blips.clear(); }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize sz = GetClientSize();
        const double r  = std::min(sz.x, sz.y) / 2.0 - FromDIP(4);
        const double cx = sz.x / 2.0, cy = sz.y / 2.0;
        gc->SetPen(wxPen(wxColour(42, 58, 47), 1));
        gc->SetBrush(wxBrush(wxColour(14, 16, 18)));
        gc->DrawEllipse(cx - r, cy - r, 2 * r, 2 * r);
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        for (double k : {0.68, 0.36})
            gc->DrawEllipse(cx - r * k, cy - r * k, 2 * r * k, 2 * r * k);
        gc->StrokeLine(cx - r, cy, cx + r, cy);
        gc->StrokeLine(cx, cy - r, cx, cy + r);
        // wiązka: wycinek 60° zanikający za linią
        const double a = m_angle * kPi / 180.0;
        for (int i = 0; i < 30; ++i) {
            const double a0 = a - (i + 1) * (kPi / 90.0), a1 = a - i * (kPi / 90.0);
            wxGraphicsPath p = gc->CreatePath();
            p.MoveToPoint(cx, cy);
            p.AddArc(cx, cy, r, a0, a1, true);
            p.CloseSubpath();
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(62, 214, 99, int(110 * (1.0 - i / 30.0)))));
            gc->FillPath(p);
        }
        gc->SetPen(wxPen(kGreen, 2));
        gc->StrokeLine(cx, cy, cx + r * std::cos(a), cy + r * std::sin(a));
        // punkty maszyn: jaśniejsze tuż po przejściu wiązki
        for (const auto& b : m_blips) {
            const double ba   = b.angle * kPi / 180.0;
            double       diff = std::fmod(m_angle - b.angle + 720.0, 360.0);
            const int    alpha = int(255 - std::min(diff, 300.0) * 0.5);
            const double bx = cx + r * b.dist * std::cos(ba), by = cy + r * b.dist * std::sin(ba), d = FromDIP(6);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(62, 214, 99, std::max(alpha, 110))));
            gc->DrawEllipse(bx - d, by - d, 2 * d, 2 * d);
        }
        gc->SetBrush(wxBrush(kGreen));
        gc->DrawEllipse(cx - FromDIP(4), cy - FromDIP(4), FromDIP(8), FromDIP(8));
    }

    struct Blip { double angle, dist; };
    wxTimer           m_timer;
    double            m_angle = 0;
    std::vector<Blip> m_blips;
};

// ---------------------------------------------------------------- Karta znalezionej maszyny
class PrinterCard : public wxPanel
{
public:
    PrinterCard(wxWindow* parent, const FoundPrinter& f, std::function<void(PrinterCard*)> on_click)
        : wxPanel(parent, wxID_ANY), m_on_click(std::move(on_click))
    {
        SetBackgroundColour(kCard);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_thumb   = new wxStaticBitmap(this, wxID_ANY, model_thumb(this, f.model));
        row->Add(m_thumb, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));
        auto* col = new wxBoxSizer(wxVERTICAL);
        auto* top = new wxBoxSizer(wxHORIZONTAL);
        m_name    = new wxStaticText(this, wxID_ANY, model_name(f.model));
        m_name->SetFont(m_name->GetFont().Bold().Larger());
        m_name->SetForegroundColour(kText);
        m_state = new wxStaticText(this, wxID_ANY, "");
        top->Add(m_name, 0, wxALIGN_CENTER_VERTICAL);
        top->Add(m_state, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        col->Add(top, 0, wxBOTTOM, FromDIP(2));
        m_dims = new wxStaticText(this, wxID_ANY, model_dims(f.model));
        m_id   = new wxStaticText(this, wxID_ANY, "");
        m_net  = new wxStaticText(this, wxID_ANY, "");
        for (auto* t : {m_dims, m_id})
            t->SetForegroundColour(kText);
        m_net->SetForegroundColour(kMuted);
        for (auto* t : {m_dims, m_id, m_net})
            col->Add(t, 0, wxBOTTOM, FromDIP(1));
        row->Add(col, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(10));
        SetSizer(row);
        update(f);
        for (wxWindow* w : std::initializer_list<wxWindow*>{this, m_thumb, m_name, m_state, m_dims, m_id, m_net})
            w->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { m_on_click(this); });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(this);
            const wxSize s = GetClientSize();
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(m_selected ? kGreen : kLine, m_selected ? FromDIP(2) : 1));
            dc.DrawRoundedRectangle(0, 0, s.x, s.y, FromDIP(8));
        });
    }

    void update(const FoundPrinter& f)
    {
        m_f = f;
        wxString st = state_name(f.state);
        if (f.state == "drukowanie" && f.progress > 0)
            st += wxString::Format(" %d%%", f.progress);
        m_state->SetLabel(wxString::FromUTF8("● ") + st + (m_added ? "   " + _L("✓ Added") : wxString()));
        m_state->SetForegroundColour(state_colour(f.state));
        // TRN %1% serial number, %2% nozzle diameter in mm
        m_id->SetLabel(format_wxstr(_L("S/N %1% · Nozzle %2% mm"), f.id, f.nozzle > 0 ? wxString::Format("%.1f", f.nozzle) : wxString("?")));
        m_net->SetLabel(from_u8(f.ip) + (f.os.empty() ? wxString() : " · Revo OS " + from_u8(f.os)));
        Layout();
    }
    void set_selected(bool s) { m_selected = s; Refresh(); }
    void set_added(bool a) { m_added = a; update(m_f); }
    bool added() const { return m_added; }
    const FoundPrinter& printer() const { return m_f; }

private:
    std::function<void(PrinterCard*)> m_on_click;
    FoundPrinter                      m_f;
    wxStaticBitmap*                   m_thumb;
    wxStaticText *                    m_name, *m_state, *m_dims, *m_id, *m_net;
    bool                              m_selected = false, m_added = false;
};

// ---------------------------------------------------------------- Ekran „Find your Revon printer”
// Radar + karty maszyn znalezionych w sieci lokalnej (szukanie trwa, dopóki okno jest otwarte). Klik karty = wybór,
// „Add …” dodaje maszynę jako obsługiwaną (profil maszyny i dyszy, wysyłanie wydruków). Adres można też wpisać ręcznie.
class ConnectWizard : public wxDialog
{
public:
    explicit ConnectWizard(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("[r]evo – find your Revon printer"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    {
        SetBackgroundColour(kBg);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* h = heading(this, _L("Find your Revon printer"));
        h->SetForegroundColour(kText);
        s->Add(h, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
        m_sub = new wxStaticText(this, wxID_ANY, _L("Searching the local network… Printers appear as soon as they answer."));
        m_sub->SetForegroundColour(kMuted);
        s->Add(m_sub, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20) / 4);
        s->AddSpacer(FromDIP(14));

        auto* mid = new wxBoxSizer(wxHORIZONTAL);
        m_radar   = new RadarPanel(this);
        mid->Add(m_radar, 0, wxLEFT | wxRIGHT, FromDIP(20));
        m_list = new wxScrolledWindow(this, wxID_ANY);
        m_list->SetBackgroundColour(kBg);
        m_list->SetScrollRate(0, FromDIP(10));
        m_list->SetMinSize(FromDIP(wxSize(440, 250)));
        m_list_sizer = new wxBoxSizer(wxVERTICAL);
        m_list->SetSizer(m_list_sizer);
        mid->Add(m_list, 1, wxEXPAND | wxRIGHT, FromDIP(20));
        s->Add(mid, 1, wxEXPAND);

        // ręczny adres (sieć, w której wyszukiwanie nie działa) i logowanie kierownika, gdy maszyna go wymaga
        auto* manual = new wxHyperlinkCtrl(this, wxID_ANY, _L("Not on the list? Enter the address manually"), "");
        manual->SetNormalColour(kMuted);
        manual->SetVisitedColour(kMuted);
        manual->Bind(wxEVT_HYPERLINK, [this](wxHyperlinkEvent&) { show_manual(!m_manual->IsShown()); });
        s->Add(manual, 0, wxLEFT | wxTOP, FromDIP(20));
        m_manual = new wxPanel(this);
        auto* ms = new wxBoxSizer(wxVERTICAL);
        m_host   = new wxTextCtrl(m_manual, wxID_ANY, "");
        m_login  = new wxTextCtrl(m_manual, wxID_ANY);
        m_pin    = new wxTextCtrl(m_manual, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_host->SetHint(_L("Printer address, e.g. revon.local or 192.168.0.38"));
        m_login->SetHint(_L("Manager login (only if the printer requires it)"));
        m_pin->SetHint(_L("PIN (only if the printer requires it)"));
        m_host->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { if (!m_host->IsEmpty()) select(nullptr); update_add(); e.Skip(); });
        for (auto* c : {m_host, m_login, m_pin})
            ms->Add(c, 0, wxEXPAND | wxTOP, FromDIP(6));
        m_manual->SetSizer(ms);
        m_manual->Hide();
        s->Add(m_manual, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

        m_msg = new wxStaticText(this, wxID_ANY, "");
        s->Add(m_msg, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
        auto* btns = new wxBoxSizer(wxHORIZONTAL);
        auto* again = new wxButton(this, wxID_ANY, _L("Search again"));
        again->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { restart(); });
        auto* close = new wxButton(this, wxID_CANCEL, _L("Close"));
        m_add = new wxButton(this, wxID_ANY, _L("Add printer"));
        m_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add(); });
        m_add->Disable();
        btns->Add(again, 0, wxRIGHT, FromDIP(8));
        btns->AddStretchSpacer();
        btns->Add(close, 0, wxRIGHT, FromDIP(8));
        btns->Add(m_add, 0);
        s->Add(btns, 0, wxEXPAND | wxALL, FromDIP(20));
        SetSizerAndFit(s);
        CentreOnParent();
        CallAfter([this] { restart(); });
    }

    ~ConnectWizard() override { *m_stop = true; } // wątek szukania kończy się sam, wyniki do zamkniętego okna nie trafią

private:
    // Szukanie w tle, powtarzane co kilka sekund, dopóki okno jest otwarte (karty pojawiają się i odświeżają na bieżąco)
    void restart()
    {
        *m_stop = true;
        m_stop  = std::make_shared<std::atomic<bool>>(false);
        ConnectWizard* self = this;
        auto           stop = m_stop;
        std::thread([self, stop] {
            while (!*stop) {
                discover_printers([self, stop](const FoundPrinter& f) {
                    wxTheApp->CallAfter([self, stop, f] { if (!*stop) self->on_found(f); });
                }, stop);
                wxTheApp->CallAfter([self, stop] { if (!*stop) self->on_round_done(); });
                for (int i = 0; i < 50 && !*stop; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }

    void on_found(const FoundPrinter& f)
    {
        for (auto* c : m_cards)
            if (c->printer().id == f.id) {
                c->update(f);
                return;
            }
        m_radar->add_blip(f.id);
        auto* c = new PrinterCard(m_list, f, [this](PrinterCard* c) { select(c); });
        c->set_added(already_added(f.ip));
        m_cards.push_back(c);
        m_list_sizer->Add(c, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
        m_list->FitInside();
        m_list->Layout();
        if (m_cards.size() == 1 && !c->added())
            select(c); // pierwsza znaleziona od razu zaznaczona
        update_sub();
    }

    void on_round_done()
    {
        m_rounds++;
        update_sub();
    }

    void update_sub()
    {
        if (m_cards.empty())
            m_sub->SetLabel(m_rounds == 0 ? _L("Searching the local network… Printers appear as soon as they answer.")
                                          : _L("Still searching… Check that the printer is on and in the same network. "
                                               "If your computer asks for access to the local network, allow it."));
        else
            m_sub->SetLabel(format_wxstr(_L_PLURAL("Found %1% printer. Click it and add it to [r]evo Slicer.",
                                                   "Found %1% printers. Click one and add it to [r]evo Slicer.", m_cards.size()),
                                         m_cards.size()));
        m_sub->Wrap(FromDIP(700));
        Layout();
    }

    void select(PrinterCard* c)
    {
        m_sel = c;
        for (auto* x : m_cards)
            x->set_selected(x == c);
        if (c && !m_host->IsEmpty()) {
            m_host->ChangeValue("");
        }
        update_add();
    }

    void update_add()
    {
        if (m_sel) {
            // TRN %1% printer model name, e.g. "Revon Belt 350"
            m_add->SetLabel(format_wxstr(_L("Add %1%"), model_name(m_sel->printer().model)));
            m_add->Enable(true);
        } else {
            m_add->SetLabel(_L("Add printer"));
            m_add->Enable(!m_host->GetValue().Trim().IsEmpty());
        }
        Layout();
    }

    void show_manual(bool on)
    {
        m_manual->Show(on);
        Layout();
        Fit();
    }

    void message(const wxString& t, bool error)
    {
        m_msg->SetForegroundColour(error ? wxColour(255, 77, 82) : kGreen);
        m_msg->SetLabel(t);
        m_msg->Wrap(FromDIP(700));
        Layout();
        Fit();
    }

    // Czy maszyna o tym adresie jest już dodana (profil drukarki z takim adresem wysyłania)
    static bool already_added(const std::string& ip)
    {
        for (const Preset& p : wxGetApp().preset_bundle->printers)
            if (!p.is_system && !p.is_default)
                if (const auto* h = p.config.option<ConfigOptionString>("print_host"); h && h->value == ip)
                    return true;
        return false;
    }

    void add()
    {
        const std::string host = m_sel ? m_sel->printer().ip : into_u8(m_host->GetValue().Trim().Trim(false));
        if (host.empty())
            return;
        wxString err;
        bool     need_login = false;
        if (!connect(host, into_u8(m_login->GetValue()), into_u8(m_pin->GetValue()), err, need_login)) {
            if (need_login) {
                show_manual(true);
                m_login->SetFocus();
            }
            return message(err, true);
        }
        if (m_sel)
            m_sel->set_added(true);
        message(_L("Printer added. Send prints with the Print button. You can add another printer or close this window."), false);
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

    // Dodaje maszynę jako obsługiwaną: klucz API (bez logowania albo login/PIN kierownika), profil maszyny i dyszy,
    // adres wysyłania wydruków. false + komunikat w err, gdy się nie da.
    bool connect(std::string host, const std::string& login, const std::string& pin, wxString& err_out, bool& need_login_out)
    {
        wxBusyCursor busy;
        auto fail = [&err_out](const wxString& m) { err_out = m; return false; };
        if (host.rfind("http", 0) != 0)
            host = "http://" + host;
        while (!host.empty() && host.back() == '/')
            host.pop_back();
        std::string cookie, key, err;
        unsigned    st = 0;
        if (login.empty()) {
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
                need_login_out = true;
                return fail(_L("This printer requires the manager login. Enter the login and PIN."));
            }
            if (key.empty())
                return fail(from_u8(err.empty() ? _u8L("The printer did not give the access key. Update Revo OS on the printer.") : err));
        } else {
            nlohmann::json body = {{"login", login}, {"pin", pin}};
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
            return false;
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
        return true;
    }

    RadarPanel*               m_radar;
    wxScrolledWindow*         m_list;
    wxBoxSizer*               m_list_sizer;
    wxStaticText *            m_sub, *m_msg;
    wxPanel*                  m_manual;
    wxTextCtrl *              m_host, *m_login, *m_pin;
    wxButton*                 m_add;
    std::vector<PrinterCard*> m_cards;
    PrinterCard*              m_sel    = nullptr;
    int                       m_rounds = 0;
    std::shared_ptr<std::atomic<bool>> m_stop = std::make_shared<std::atomic<bool>>(false);
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
