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
#include <wx/cursor.h>
#include <wx/font.h>
#include <wx/dcclient.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
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
    img = img.ConvertToGreyscale();
    const int px = w->FromDIP(96);
    return wxBitmap(img.Rescale(px, px * img.GetHeight() / std::max(1, img.GetWidth()), wxIMAGE_QUALITY_HIGH));
}

wxString model_dims(const std::string& m)
{
    if (m == "belt") return wxString::FromUTF8("350×350×300");
    if (m == "600x600x400") return wxString::FromUTF8("600×600×400");
    if (m == "600x600x600") return wxString::FromUTF8("600×600×600");
    if (m == "1200x600x500") return wxString::FromUTF8("1200×600×500");
    return "–";
}

constexpr double kPi = 3.14159265358979323846;

// Kolory ekranu wyszukiwania (styl startu rakiety: czerń, biel, chłodny błękit horyzontu)
const wxColour kBg(0, 0, 0), kLine(42, 47, 54), kText(255, 255, 255), kMuted(154, 164, 178), kBlue(90, 160, 255), kGreen(127, 209, 139);

// Rozstrzelone wielkie litery (napisy w stylu telemetrii); tłumaczenie zostaje zwykłym tekstem
wxString spaced(const wxString& t, int gap = 1)
{
    wxString out;
    const wxString sep(wxUniChar(0x2009), gap); // cienka spacja
    const wxString up = t.Upper();
    for (size_t i = 0; i < up.length(); ++i) {
        if (i)
            out += sep;
        out += up[i];
    }
    return out;
}

wxStaticText* label(wxWindow* p, const wxString& t, int pt, const wxColour& c, bool light = false)
{
    auto*  l = new wxStaticText(p, wxID_ANY, t);
    wxFont f = l->GetFont();
    f.SetPointSize(pt);
    if (light)
        f.SetWeight(wxFONTWEIGHT_LIGHT);
    l->SetFont(f);
    l->SetForegroundColour(c);
    l->SetBackgroundColour(kBg);
    return l;
}

// ---------------------------------------------------------------- Płaski przycisk (DOCK / UNDOCK): ramka, tekst, podświetlenie
class FlatButton : public wxPanel
{
public:
    FlatButton(wxWindow* parent, const wxString& text, bool primary, std::function<void()> on_click)
        : wxPanel(parent, wxID_ANY), m_text(text), m_primary(primary), m_click(std::move(on_click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        wxFont f = GetFont();
        f.SetPointSize(primary ? 12 : 10);
        SetFont(f);
        wxClientDC dc(this);
        dc.SetFont(f);
        const wxSize t = dc.GetTextExtent(m_text);
        SetMinSize(wxSize(t.x + FromDIP(primary ? 44 : 32), t.y + FromDIP(primary ? 18 : 12)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            const wxSize          s = GetClientSize();
            dc.SetBackground(wxBrush(m_hover ? wxColour(m_primary ? 255 : 90, m_primary ? 255 : 100, m_primary ? 255 : 114) : kBg));
            dc.Clear();
            dc.SetPen(wxPen(m_primary ? kText : wxColour(90, 100, 114), 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(0, 0, s.x, s.y);
            dc.SetFont(GetFont());
            dc.SetTextForeground(m_hover ? (m_primary ? kBg : kText) : (m_primary ? kText : kMuted));
            const wxSize t = dc.GetTextExtent(m_text);
            dc.DrawText(m_text, (s.x - t.x) / 2, (s.y - t.y) / 2);
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (auto f = m_click) wxTheApp->CallAfter([f] { f(); }); }); // przycisk może zniknąć po kliknięciu
    }

private:
    wxString              m_text;
    bool                  m_primary, m_hover = false;
    std::function<void()> m_click;
};

// ---------------------------------------------------------------- Horyzont: łuk Ziemi, znacznik skanu ze smugą, błysk przy znalezieniu
class HorizonPanel : public wxPanel
{
public:
    HorizonPanel(wxWindow* parent, std::function<void()> on_manual) : wxPanel(parent, wxID_ANY), m_manual(std::move(on_manual))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, FromDIP(120)));
        Bind(wxEVT_PAINT, &HorizonPanel::on_paint, this);
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { SetCursor(wxCursor(m_link.Contains(e.GetPosition()) ? wxCURSOR_HAND : wxCURSOR_ARROW)); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) { if (m_link.Contains(e.GetPosition()) && m_manual) m_manual(); });
    }

    void set_time(double t) { m_t = t; Refresh(false); }
    void flash() { m_flash_t = m_t; m_flash_u = phase(); }
    void set_scanning(bool on) { m_scanning = on; }

private:
    static constexpr double kPeriod = 4.0;
    double phase() const { return std::fmod(m_t, kPeriod) / kPeriod; }

    // punkt łuku (krzywa Béziera przez środek u góry), u = 0…1 zakres skanu
    wxPoint2DDouble arc(double u, double off = 0) const
    {
        const wxSize sz = GetClientSize();
        const double k = 0.15 + 0.7 * u, a = -0.11 * sz.x, c = sz.x * 1.11, y0 = sz.y + FromDIP(30) + off, y1 = FromDIP(-10) + off;
        return {(1 - k) * (1 - k) * a + 2 * k * (1 - k) * sz.x / 2.0 + k * k * c, (1 - k) * (1 - k) * y0 + 2 * k * (1 - k) * y1 + k * k * y0};
    }

    void stroke_arc(wxGraphicsContext* gc, double off)
    {
        wxGraphicsPath p = gc->CreatePath();
        for (int i = 0; i <= 60; ++i) {
            const double u = -0.22 + 1.44 * i / 60.0;
            const auto   q = arc(u, off);
            i ? p.AddLineToPoint(q) : p.MoveToPoint(q);
        }
        gc->StrokePath(p);
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(kBg));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize sz = GetClientSize();
        // poświata atmosfery i dwie linie horyzontu
        gc->SetPen(wxPen(wxColour(90, 160, 255, 60), FromDIP(6)));
        stroke_arc(gc.get(), FromDIP(14));
        gc->SetPen(wxPen(wxColour(255, 255, 255, 90), 1));
        stroke_arc(gc.get(), 0);
        if (m_scanning) {
            // smuga za znacznikiem
            const double u = phase();
            for (int i = 0; i < 24; ++i) {
                const double v0 = std::max(0.0, u - 0.25 * (1 - i / 24.0)), v1 = std::max(0.0, u - 0.25 * (1 - (i + 1) / 24.0));
                gc->SetPen(wxPen(wxColour(159, 208, 255, int(230 * i / 24.0)), FromDIP(3)));
                const auto a = arc(v0), b = arc(v1);
                gc->StrokeLine(a.m_x, a.m_y, b.m_x, b.m_y);
            }
            const auto m = arc(u);
            gc->SetPen(wxPen(kText, 1));
            gc->StrokeLine(m.m_x, m.m_y - FromDIP(14), m.m_x, m.m_y + FromDIP(6));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(kText));
            gc->DrawEllipse(m.m_x - FromDIP(3), m.m_y - FromDIP(3), FromDIP(6), FromDIP(6));
        }
        // błysk w miejscu wykrycia
        const double fd = m_t - m_flash_t;
        if (m_flash_t >= 0 && fd < 0.8) {
            const double k = 1 - fd / 0.8, r = FromDIP(20 + 40 * (1 - k));
            const auto   f = arc(m_flash_u);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(gc->CreateRadialGradientBrush(f.m_x, f.m_y, f.m_x, f.m_y, r, wxColour(255, 255, 255, int(255 * k)), wxColour(90, 160, 255, 0)));
            gc->DrawEllipse(f.m_x - r, f.m_y - r, 2 * r, 2 * r);
        }
        // SCANNING ● ● ● (fala) i link do adresu ręcznego
        wxFont f = GetFont();
        f.SetPointSize(9);
        gc->SetFont(f, kMuted);
        const wxString sc = spaced(m_scanning ? _L("Scanning") : _L("Paused"), 2);
        double w, h;
        gc->GetTextExtent(sc, &w, &h);
        const double dots = FromDIP(m_scanning ? 40 : 0), x0 = (sz.x - w - dots) / 2, y = sz.y - FromDIP(26);
        gc->DrawText(sc, x0, y);
        if (m_scanning)
            for (int i = 0; i < 3; ++i) {
                const double ph = std::fmod(m_t * 1.5 - i * 0.33 + 3, 1.0), a = 0.2 + 0.8 * std::max(0.0, 1 - std::abs(ph - 0.2) * 3);
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(wxColour(255, 255, 255, int(255 * a))));
                const double cx = x0 + w + FromDIP(12) + i * FromDIP(12), r = FromDIP(2.5);
                gc->DrawEllipse(cx - r, y + h / 2 - r, 2 * r, 2 * r);
            }
        const wxString ml = spaced(_L("Manual address")) + wxString::FromUTF8(" ›");
        gc->GetTextExtent(ml, &w, &h);
        gc->DrawText(ml, FromDIP(40), y);
        m_link = wxRect(FromDIP(36), int(y) - FromDIP(6), int(w) + FromDIP(10), int(h) + FromDIP(12));
    }

    std::function<void()> m_manual;
    double                m_t = 0, m_flash_t = -1, m_flash_u = 0;
    bool                  m_scanning = true;
    wxRect                m_link;
};

// ---------------------------------------------------------------- Wiersz znalezionej maszyny: miniatura, nazwa, telemetria, DOCK/UNDOCK
class VehicleRow : public wxPanel
{
public:
    VehicleRow(wxWindow* parent, const FoundPrinter& f, std::function<void(VehicleRow*)> on_dock, std::function<void(VehicleRow*)> on_undock)
        : wxPanel(parent, wxID_ANY), m_on_dock(std::move(on_dock)), m_on_undock(std::move(on_undock))
    {
        SetBackgroundColour(kBg);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticBitmap(this, wxID_ANY, model_thumb(this, f.model)), 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(12));
        auto* nc = new wxBoxSizer(wxVERTICAL);
        m_name   = label(this, spaced(model_name(f.model)), 17, kText, true);
        m_serial = label(this, spaced(from_u8(f.id)), 9, kMuted);
        nc->Add(m_name);
        nc->Add(m_serial, 0, wxTOP, FromDIP(2));
        nc->SetMinSize(wxSize(FromDIP(230), -1));
        row->Add(nc, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(18));
        auto field = [&](const wxString& cap, int min_w) {
            auto* c = new wxBoxSizer(wxVERTICAL);
            c->Add(label(this, spaced(cap, 2), 8, kMuted));
            auto* v = label(this, "", 15, kText, true);
            c->Add(v, 0, wxTOP, FromDIP(2));
            c->SetMinSize(wxSize(FromDIP(min_w), -1));
            row->Add(c, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
            return v;
        };
        m_status = field(_L("Status"), 150);
        m_nozzle = field(_L("Nozzle"), 70);
        m_build  = field(_L("Build"), 120);
        m_addr   = field(_L("Address"), 110);
        row->AddStretchSpacer();
        m_action = new wxBoxSizer(wxVERTICAL);
        row->Add(m_action, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        auto* line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        line->SetBackgroundColour(kLine);
        outer->Add(line, 0, wxEXPAND);
        SetSizer(outer);
        update(f);
        set_docked(false);
    }

    void update(const FoundPrinter& f)
    {
        m_f = f;
        wxString st = state_name(f.state);
        if (f.state == "drukowanie" && f.progress > 0)
            st += wxString::Format(" %d%%", f.progress);
        m_status->SetLabel(st.Upper());
        m_status->SetForegroundColour(f.state == "blad" ? wxColour(255, 77, 82) : kText);
        m_nozzle->SetLabel(f.nozzle > 0 ? wxString::Format("%.1f", f.nozzle) : wxString("?"));
        m_build->SetLabel(model_dims(f.model));
        m_addr->SetLabel(from_u8(f.ip));
        m_addr->SetToolTip(f.os.empty() ? wxString() : "Revo OS " + from_u8(f.os));
        Layout();
    }

    void set_docked(bool d)
    {
        m_docked = d;
        m_action->Clear(true);
        if (d) {
            m_action->Add(label(this, wxString::FromUTF8("✓ ") + spaced(_L("Docked")), 8, kGreen), 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(5));
            m_action->Add(new FlatButton(this, spaced(_L("Undock"), 2), false, [this] { m_on_undock(this); }), 0, wxALIGN_CENTER);
        } else
            m_action->Add(new FlatButton(this, spaced(_L("Dock"), 2), true, [this] { m_on_dock(this); }), 0, wxALIGN_CENTER);
        Layout();
        GetParent()->Layout();
    }

    // podświetlenie świeżo znalezionej maszyny (1 → 0)
    void set_glow(double k)
    {
        const wxColour c(int(kBlue.Red() * 0.18 * k), int(kBlue.Green() * 0.18 * k), int(kBlue.Blue() * 0.18 * k));
        if (c == m_glow)
            return;
        m_glow = c;
        SetBackgroundColour(c);
        for (wxWindow* w : GetChildren())
            if (dynamic_cast<wxStaticText*>(w))
                w->SetBackgroundColour(c);
        Refresh();
    }

    bool                docked() const { return m_docked; }
    const FoundPrinter& printer() const { return m_f; }
    double              found_at = 0;

private:
    std::function<void(VehicleRow*)> m_on_dock, m_on_undock;
    FoundPrinter                     m_f;
    wxStaticText *                   m_name, *m_serial, *m_status, *m_nozzle, *m_build, *m_addr;
    wxBoxSizer*                      m_action;
    wxColour                         m_glow = kBg;
    bool                             m_docked = false;
};

// ---------------------------------------------------------------- Ekran „Find your Revon”
// Maszyny znalezione w sieci lokalnej (szukanie trwa, dopóki okno jest otwarte), każda w swoim wierszu z DOCK
// (dodanie do [r]evo Slicer: profil maszyny i dyszy, wysyłanie wydruków) albo UNDOCK (usunięcie). Adres można wpisać ręcznie.
class ConnectWizard : public wxDialog
{
public:
    explicit ConnectWizard(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("[r]evo – find your Revon printer"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_timer(this)
    {
        SetBackgroundColour(kBg);
        auto* s   = new wxBoxSizer(wxVERTICAL);
        auto* top = new wxBoxSizer(wxHORIZONTAL);
        auto* tl  = new wxBoxSizer(wxVERTICAL);
        tl->Add(label(this, spaced(_L("[r]evo Slicer · network acquisition"), 3), 9, kMuted));
        tl->Add(label(this, spaced(_L("Find your Revon"), 3), 32, kText, true), 0, wxTOP, FromDIP(14));
        m_sub = label(this, "", 10, kMuted);
        tl->Add(m_sub, 0, wxTOP, FromDIP(6));
        top->Add(tl, 1);
        auto* tr = new wxBoxSizer(wxVERTICAL);
        tr->Add(label(this, spaced(_L("Scan time"), 3), 8, kMuted), 0, wxALIGN_RIGHT);
        m_clock = label(this, "T+00:00", 26, kText, true);
        tr->Add(m_clock, 0, wxALIGN_RIGHT);
        top->Add(tr, 0);
        s->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(40));
        auto* rule = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        rule->SetBackgroundColour(kText);
        s->Add(rule, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(40));

        m_list = new wxScrolledWindow(this, wxID_ANY);
        m_list->SetBackgroundColour(kBg);
        m_list->SetScrollRate(0, FromDIP(10));
        m_list->SetMinSize(FromDIP(wxSize(900, 250)));
        m_list_sizer = new wxBoxSizer(wxVERTICAL);
        m_list->SetSizer(m_list_sizer);
        s->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(40));

        // ręczny adres (sieć, w której wyszukiwanie nie działa) i logowanie kierownika, gdy maszyna go wymaga
        m_manual = new wxPanel(this);
        m_manual->SetBackgroundColour(kBg);
        auto* ms = new wxBoxSizer(wxHORIZONTAL);
        m_host   = new wxTextCtrl(m_manual, wxID_ANY, "");
        m_login  = new wxTextCtrl(m_manual, wxID_ANY);
        m_pin    = new wxTextCtrl(m_manual, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_host->SetHint(_L("Printer address, e.g. revon.local or 192.168.0.38"));
        m_login->SetHint(_L("Manager login (only if the printer requires it)"));
        m_pin->SetHint(_L("PIN (only if the printer requires it)"));
        ms->Add(m_host, 2, wxRIGHT, FromDIP(8));
        ms->Add(m_login, 2, wxRIGHT, FromDIP(8));
        ms->Add(m_pin, 1, wxRIGHT, FromDIP(12));
        ms->Add(new FlatButton(m_manual, spaced(_L("Dock"), 2), true, [this] { dock(into_u8(m_host->GetValue().Trim().Trim(false)), nullptr); }), 0, wxALIGN_CENTER_VERTICAL);
        m_manual->SetSizer(ms);
        m_manual->Hide();
        s->Add(m_manual, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(40) / 2);

        m_msg = label(this, "", 10, kGreen);
        s->Add(m_msg, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(40) / 2);
        m_horizon = new HorizonPanel(this, [this] { show_manual(!m_manual->IsShown()); });
        s->Add(m_horizon, 0, wxEXPAND | wxTOP, FromDIP(10));
        SetSizerAndFit(s);
        SetMinSize(GetSize());
        CentreOnParent();

        m_start = std::chrono::steady_clock::now();
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
        m_timer.Start(33);
        update_sub();
        CallAfter([this] { restart(); });
    }

    ~ConnectWizard() override
    {
        m_timer.Stop();
        *m_stop = true; // wątek szukania kończy się sam, wyniki do zamkniętego okna nie trafią
    }

private:
    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count(); }

    void tick()
    {
        const double t = now();
        m_horizon->set_time(t);
        const int sec = int(t);
        if (sec != m_last_sec) {
            m_last_sec = sec;
            m_clock->SetLabel(wxString::Format("T+%02d:%02d", (sec / 60) % 100, sec % 60));
        }
        for (auto* r : m_rows)
            r->set_glow(std::max(0.0, 1.0 - (t - r->found_at) / 1.2));
    }

    // Szukanie w tle, powtarzane co kilka sekund, dopóki okno jest otwarte (wiersze pojawiają się i odświeżają na bieżąco)
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
        for (auto* r : m_rows)
            if (r->printer().id == f.id) {
                r->update(f);
                return;
            }
        auto* r = new VehicleRow(m_list, f, [this](VehicleRow* r) { dock(r->printer().ip, r); }, [this](VehicleRow* r) { undock(r); });
        r->set_docked(already_added(f.ip));
        r->found_at = now();
        m_rows.push_back(r);
        m_list_sizer->Add(r, 0, wxEXPAND);
        m_list->FitInside();
        m_list->Layout();
        m_horizon->flash();
        update_sub();
    }

    void on_round_done()
    {
        m_rounds++;
        update_sub();
    }

    void update_sub()
    {
        std::string net = local_ipv4();
        if (const auto d = net.rfind('.'); d != std::string::npos)
            net = net.substr(0, d) + ".0/24";
        if (m_rows.empty())
            m_sub->SetLabel(m_rounds == 0 ? _L("Searching the local network… Printers appear as soon as they answer.")
                                          : _L("Still searching… Check that the printer is on and in the same network. "
                                               "If your computer asks for access to the local network, allow it."));
        else
            // TRN %1% number of printers found, %2% network address, e.g. 192.168.0.0/24
            m_sub->SetLabel(format_wxstr(_L_PLURAL("%1% printer found on %2%", "%1% printers found on %2%", m_rows.size()), m_rows.size(),
                                         from_u8(net.empty() ? std::string("revon.local") : net))
                                .Upper());
        m_sub->Wrap(FromDIP(700));
        Layout();
    }

    void show_manual(bool on)
    {
        m_manual->Show(on);
        if (on)
            m_host->SetFocus();
        Layout();
    }

    void message(const wxString& t, bool error)
    {
        m_msg->SetForegroundColour(error ? wxColour(255, 77, 82) : kGreen);
        m_msg->SetLabel(t);
        m_msg->Wrap(FromDIP(800));
        Layout();
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

    void dock(const std::string& host, VehicleRow* row)
    {
        if (host.empty())
            return show_manual(true);
        wxString err;
        bool     need_login = false;
        if (!connect(host, into_u8(m_login->GetValue()), into_u8(m_pin->GetValue()), err, need_login)) {
            if (need_login) {
                m_host->ChangeValue(from_u8(host));
                show_manual(true);
                m_login->SetFocus();
            }
            return message(err, true);
        }
        if (row)
            row->set_docked(true);
        else
            for (auto* r : m_rows)
                if (r->printer().ip == host)
                    r->set_docked(true);
        message(_L("Printer docked. Send prints with the Print button."), false);
    }

    // Usuwa z [r]evo Slicer profile drukarki z adresem tej maszyny (ustawienia i pliki użytkownika zostają)
    void undock(VehicleRow* row)
    {
        const std::string ip = row->printer().ip;
        // TRN %1% printer model name, e.g. "Revon Belt 350"
        MessageDialog dlg(this, format_wxstr(_L("Undock %1%? [r]evo Slicer forgets its address. You can dock it again at any time."), model_name(row->printer().model)),
                          _L("Undock printer"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (dlg.ShowModal() != wxID_YES)
            return;
        auto&                    pr = wxGetApp().preset_bundle->printers;
        std::vector<std::string> names;
        for (const Preset& p : pr)
            if (!p.is_system && !p.is_default)
                if (const auto* h = p.config.option<ConfigOptionString>("print_host"); h && h->value == ip)
                    names.push_back(p.name);
        Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
        for (const auto& n : names) {
            if (pr.get_selected_preset_name() == n && tab) {
                const Preset* parent = pr.get_selected_preset_parent();
                tab->select_preset(parent ? parent->name : pr.default_preset().name);
            }
            pr.delete_preset(n);
        }
        if (tab)
            tab->update_tab_ui();
        wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_PRINTER);
        row->set_docked(already_added(ip));
        message(_L("Printer undocked."), false);
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

    wxTimer                  m_timer;
    HorizonPanel*            m_horizon;
    wxScrolledWindow*        m_list;
    wxBoxSizer*              m_list_sizer;
    wxStaticText *           m_sub, *m_msg, *m_clock;
    wxPanel*                 m_manual;
    wxTextCtrl *             m_host, *m_login, *m_pin;
    std::vector<VehicleRow*> m_rows;
    int                      m_rounds = 0, m_last_sec = -1;
    std::chrono::steady_clock::time_point m_start;
    std::shared_ptr<std::atomic<bool>>    m_stop = std::make_shared<std::atomic<bool>>(false);
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
