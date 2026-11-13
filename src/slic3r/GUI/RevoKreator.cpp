#include <boost/algorithm/string/predicate.hpp>
#include "slic3r/GUI/I18N.hpp"

#include "RevoKreator.hpp"

#include <algorithm>
#include <map>
#include <utility>
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
#include <wx/combobox.h>
#include <wx/clrpicker.h>
#include <set>
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
#include "slic3r/GUI/WebViewDialog.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/Http.hpp"

#include <nlohmann/json.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Utils.hpp"
#include "libslic3r/calib.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/calib_dlg.hpp"
#include <wx/gdicmn.h>

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
            const double k = 1 - fd / 0.8, r = FromDIP(20) + FromDIP(40) * (1 - k);
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
                const double cx = x0 + w + FromDIP(12) + i * FromDIP(12), r = FromDIP(5) / 2.0;
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
        nc->SetMinSize(wxSize(FromDIP(210), -1));
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
        m_status = field(_L("Status"), 140);
        m_nozzle = field(_L("Nozzle"), 70);
        m_build  = field(_L("Build"), 110);
        m_addr   = field(_L("Address"), 120);
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

// ---------------------------------------------------------------- Kreator nowego filamentu (styl startu: sekwencja, wzór źle/dobrze, wybór wydrukowanych wartości)
std::map<int, Calib_Params> g_calib;   // ostatnio wydrukowane testy wg rodzaju
int                         g_flow_pass = 1;

struct WizStep
{
    wxString  seq, title, sub, q, bad, good, help;
    std::string art;
    CalibKind kind;
    int       calib_key;
    wxString  unit;
    int       digits;
};

// Kafelek wyboru / materiału: ramka, tekst, zaznaczenie na niebiesko.
class PickTile : public wxPanel
{
public:
    PickTile(wxWindow* parent, const wxString& text, bool on, int min_w, std::function<void()> click)
        : wxPanel(parent, wxID_ANY), m_text(text), m_on(on), m_click(std::move(click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        wxFont f = GetFont();
        f.SetPointSize(12);
        SetFont(f);
        SetMinSize(wxSize(FromDIP(min_w), FromDIP(46)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            const wxSize s = GetClientSize();
            dc.SetBackground(wxBrush(m_on ? wxColour(11, 22, 38) : (m_hover ? wxColour(14, 18, 24) : kBg)));
            dc.Clear();
            dc.SetPen(wxPen(m_on ? kBlue : (m_hover ? wxColour(90, 100, 114) : kLine), m_on ? 2 : 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(0, 0, s.x, s.y);
            dc.SetFont(GetFont());
            dc.SetTextForeground(m_on ? kBlue : kMuted);
            const wxString t = (m_on ? wxString::FromUTF8("\xE2\x9C\x93 ") : wxString()) + m_text;
            const wxSize ts = dc.GetTextExtent(t);
            dc.DrawText(t, (s.x - ts.x) / 2, (s.y - ts.y) / 2);
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (auto f = m_click) wxTheApp->CallAfter([f] { f(); }); });
    }

private:
    wxString              m_text;
    bool                  m_on, m_hover = false;
    std::function<void()> m_click;
};

// Duży przycisk „wydrukuj test”: niebieski, pulsuje dopóki nie zostanie kliknięty – trudno go przegapić.
class PulseButton : public wxPanel
{
public:
    PulseButton(wxWindow* parent, const wxString& text, bool pulse, std::function<void()> click)
        : wxPanel(parent, wxID_ANY), m_text(text), m_pulse(pulse), m_click(std::move(click)), m_timer(this)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        wxFont f = GetFont();
        f.SetPointSize(16);
        f.SetWeight(wxFONTWEIGHT_BOLD);
        SetFont(f);
        wxClientDC dc(this);
        dc.SetFont(f);
        const wxSize t = dc.GetTextExtent(m_text);
        SetMinSize(wxSize(t.x + FromDIP(70), t.y + FromDIP(30)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            const wxSize s = GetClientSize();
            // jasność pulsu 0..1
            const double k = m_pulse ? 0.5 + 0.5 * std::sin(m_phase) : 1.0;
            const wxColour fill(int(30 + (90 - 30) * k), int(60 + (160 - 60) * k), int(110 + (255 - 110) * k));
            dc.SetBackground(wxBrush(m_hover ? kBlue : fill));
            dc.Clear();
            dc.SetPen(wxPen(kBlue, 2));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(0, 0, s.x, s.y);
            dc.SetFont(GetFont());
            dc.SetTextForeground(k > 0.6 || m_hover ? kBg : kText);
            const wxSize t = dc.GetTextExtent(m_text);
            dc.DrawText(m_text, (s.x - t.x) / 2, (s.y - t.y) / 2);
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { m_phase += 0.35; Refresh(); });
        if (m_pulse) m_timer.Start(60);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            m_timer.Stop();
            m_pulse = false;
            if (auto f = m_click) wxTheApp->CallAfter([f] { f(); });
        });
    }
    ~PulseButton() override { m_timer.Stop(); }

private:
    wxString              m_text;
    bool                  m_pulse, m_hover = false;
    double                m_phase = 0;
    std::function<void()> m_click;
    wxTimer               m_timer;
};

class NewFilamentWizard : public wxDialog
{
public:
    explicit NewFilamentWizard(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("[r]evo – new filament"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxSTAY_ON_TOP)
    {
        SetBackgroundColour(kBg);
        const auto& preset = wxGetApp().preset_bundle->filaments.get_edited_preset();
        const auto& cfg    = preset.config;
        auto get = [&](const char* k, double d) {
            const ConfigOption* o = cfg.option(k);
            if (auto* f = dynamic_cast<const ConfigOptionFloats*>(o); f && !f->values.empty()) return f->values.front();
            if (auto* f = dynamic_cast<const ConfigOptionFloatsNullable*>(o); f && !f->values.empty() && !std::isnan(f->values.front())) return f->values.front();
            if (auto* i = dynamic_cast<const ConfigOptionInts*>(o); i && !i->values.empty()) return double(i->values.front());
            return d;
        };
        m_cur  = {get("nozzle_temperature", 220), get("filament_flow_ratio", 0.98), get("filament_max_volumetric_speed", 16),
                  get("pressure_advance", 0.04), get("filament_retraction_length", 0.8)};
        m_type = cfg.opt_string("filament_type", 0u);
        m_brand.clear(); // własna marka – użytkownik wpisuje albo wybiera z listy
        m_name.clear();

        m_steps = {
            {_L("Temperature"), _L("Which part of the tower looks cleanest?"),
             _L("Take the printed tower in your hand. Look at it from the side, in good light."),
             _L("Tap the part that looks most like GOOD:"), _L("Thin hairs between the pillars, droopy bridge, rough surface."),
             _L("No hairs, straight bridge, smooth shiny walls."), _L("Not sure? Pick the middle one - it will still print fine."),
             "temp", CalibKind::Temperature, int(CalibMode::Calib_Temp_Tower), wxString::FromUTF8("\xC2\xB0""C"), 0},
            {_L("Flow ratio"), _L("Which square has the smoothest top?"),
             _L("Run your fingertip across the top of each printed square."), _L("Tap the square that feels smoothest:"),
             _L("Visible gaps between lines, or rough ridges you can feel."), _L("Flat, closed surface, feels like glass."),
             _L("Not sure? Pick the middle one - it will still print fine."), "flow", CalibKind::FlowRatio,
             int(CalibMode::Calib_Flow_Rate), "%", 0},
            {_L("Max flow"), _L("Where does the wall start to tear?"), _L("Look at the tall wall from the side, from bottom to top."),
             _L("Tap the LAST band that still looks good:"), _L("Torn, frayed layers, holes in the wall."),
             _L("Even, solid layers all the way."), _L("Not sure? Pick one lower - safer, a bit slower."), "mf",
             CalibKind::MaxVolumetric, int(CalibMode::Calib_Vol_speed_Tower), wxString::FromUTF8("mm\xC2\xB3/s"), 0},
            {_L("Pressure adv."), _L("Which line has no holes?"), _L("Look closely where each line changes speed - the middle part."),
             _L("Tap the line that looks most even:"), _L("A hole or thin spot in the middle of the line."),
             _L("One even line, same width everywhere."), _L("Not sure? Pick the middle one - it will still print fine."), "pa",
             CalibKind::PressureAdvance, int(CalibMode::Calib_PA_Line), "", 3},
            {_L("Retraction"), _L("Where do the hairs disappear?"), _L("Look between the two towers, from bottom to top."),
             _L("Tap the LOWEST band with no hairs:"), _L("Thin hairs / strings between the towers."),
             _L("Clean air between the towers."), _L("Not sure? Pick one higher - no hairs is what matters."), "ret",
             CalibKind::Retraction, int(CalibMode::Calib_Retraction_tower), "mm", 1},
        };
        m_sel.assign(m_steps.size(), -1);
        m_printed.assign(m_steps.size(), false);

        auto* root = new wxBoxSizer(wxVERTICAL);
        m_head = new wxPanel(this);
        m_head->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_head->SetMinSize(FromDIP(wxSize(960, 170)));
        m_head->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_head(); });
        root->Add(m_head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(24));
        auto* mid = new wxBoxSizer(wxHORIZONTAL);
        m_body = new wxPanel(this);
        m_body->SetBackgroundColour(kBg);
        m_side = new wxPanel(this);
        m_side->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_side->SetMinSize(FromDIP(wxSize(260, 330)));
        m_side->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_side(); });
        mid->Add(m_body, 1, wxEXPAND | wxRIGHT, FromDIP(20));
        mid->Add(m_side, 0, wxEXPAND);
        root->Add(mid, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
        auto* bar = new wxBoxSizer(wxHORIZONTAL);
        bar->Add(new FlatButton(this, spaced(_L("Back")), true, [this] { show(m_page - 1); }), 0, wxALIGN_CENTER_VERTICAL);
        m_help = label(this, "", 11, kMuted);
        bar->Add(m_help, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(24));
        m_next_box = new wxBoxSizer(wxHORIZONTAL);
        bar->Add(m_next_box, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(bar, 0, wxEXPAND | wxALL, FromDIP(24));
        SetSizer(root);
        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
            e.Skip();
            if (e.GetActive() && is_step())
                CallAfter([this] { show(m_page); }); // po wydruku testu pokaż jego wartości
        });
        show(0);
        SetMinSize(FromDIP(wxSize(1040, 720)));
        Fit();
        CentreOnParent();
    }

private:
    int  pages() const { return int(m_steps.size()) + 2; }
    bool is_step() const { return m_page >= 1 && m_page <= int(m_steps.size()); }
    wxString seq_name(int i) const
    {
        if (i == 0) return _L("Setup");
        if (i == pages() - 1) return _L("Save");
        return m_steps[i - 1].seq;
    }

    // Wartości wydrukowane w ostatnim teście (od góry wydruku), albo rozsądny zakres wokół obecnej.
    std::vector<double> options(int s) const
    {
        const WizStep& st = m_steps[s];
        std::vector<double> v;
        auto range = [&v](double a, double b, double step) {
            step = std::abs(step);
            if (step <= 0) return;
            const int n = std::min(60, int(std::lround(std::abs(b - a) / step)) + 1);
            const double d = b >= a ? step : -step;
            for (int i = 0; i < n; ++i) { const double x = a + d * i; v.push_back(std::abs(x) < 1e-9 ? 0.0 : x); } // bez „-0.0”
        };
        if (s == 1) { // przepływ: płytki z przesunięciem w %
            if (g_flow_pass == 2) range(0, -9, 1); else range(20, -20, 5);
            return v;
        }
        auto it = g_calib.find(st.calib_key);
        if (it != g_calib.end() && m_printed[s]) {
            const Calib_Params& p = it->second;
            if (s == 0) range(std::min(p.start, p.end), std::max(p.start, p.end), p.step);       // wieża: góra = najchłodniej
            else if (s == 3) range(p.start, p.end, p.step);                                     // linie PA po kolei
            else range(std::max(p.start, p.end), std::min(p.start, p.end), p.step);             // wieże: góra = najwięcej
        }
        if (v.empty()) {
            const double c = m_cur[s == 0 ? 0 : s == 2 ? 2 : s == 3 ? 3 : 4];
            if (s == 0) { // zakres wieży dla gatunku (jak w oknie kalibracji temperatury)
                const std::string& m = m_type;
                double hi = 230, lo = 190;
                if (m == "ABS" || m == "ASA") { hi = 270; lo = 230; }
                else if (m == "PETG") { hi = 250; lo = 230; }
                else if (m == "TPU") { hi = 240; lo = 210; }
                else if (m == "PA-CF" || m == "PA-GF" || m == "PET-CF") { hi = 320; lo = 280; }
                range(lo, hi, 5);
            }
            else if (s == 2) { // max flow wg gatunku (jak w oknie testu), góra wieży = najwięcej
                double lo = 10, hi = 35;
                if (m_type == "PETG" || m_type == "ABS" || m_type == "ASA") { lo = 8; hi = 30; }
                else if (m_type == "TPU") { lo = 2; hi = 12; }
                else if (m_type == "PA-CF" || m_type == "PA-GF" || m_type == "PET-CF") { lo = 6; hi = 25; }
                range(hi, lo, 1);
            }
            else if (s == 3) { // PA wg gatunku – flex potrzebuje dużo więcej
                if (m_type == "TPU") range(0, 0.3, 0.01);
                else range(0, m_type == "PETG" ? 0.1 : 0.08, 0.005);
            }
            else range(c + 1.0, std::max(0.2, c - 1.0), 0.2); // retrakcja zawsze > 0
        }
        return v;
    }

    wxString fmt_val(int s, double v) const
    {
        const WizStep& st = m_steps[s];
        if (s == 1) return wxString::Format("%+d %%", int(std::lround(v)));
        wxString t = wxString::FromDouble(v, st.digits);
        return st.unit.empty() || s == 0 ? (s == 0 ? t + wxString::FromUTF8("\xC2\xB0") : t) : t + " " + st.unit;
    }

    double value(int s) const
    {
        if (m_sel[s] < 0) return s == 1 ? m_cur[1] : m_cur[s == 0 ? 0 : s];
        const double v = options(s)[std::min(size_t(m_sel[s]), options(s).size() - 1)];
        return s == 1 ? m_cur[1] * (1.0 + v / 100.0) : v;
    }

    wxString profile_val(int s) const
    {
        if (m_sel[s] < 0) return wxString::FromUTF8("\xE2\x80\x94");
        if (s == 1) return fmt_val(1, options(1)[size_t(m_sel[1])]);
        return fmt_val(s, value(s));
    }

    void paint_head()
    {
        wxAutoBufferedPaintDC dc(m_head);
        const wxSize sz = m_head->GetClientSize();
        dc.SetBackground(wxBrush(kBg));
        dc.Clear();
        wxString title, sub;
        if (m_page == 0) { title = _L("What filament are you loading?"); sub = _L("Read it from the box or the spool label."); }
        else if (is_step()) { title = m_steps[m_page - 1].title; sub = m_steps[m_page - 1].sub; }
        else { title = _L("Your filament is ready"); sub = _L("We save everything you picked as a new profile."); }
        wxFont f = m_head->GetFont();
        f.SetPointSize(9);
        dc.SetFont(f);
        dc.SetTextForeground(kMuted);
        dc.DrawText(spaced(wxString::Format(_L("New filament · step %d of %d"), m_page + 1, pages()), 2), 0, 0);
        f.SetPointSize(26);
        f.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(f);
        dc.SetTextForeground(kText);
        dc.DrawText(title, 0, m_head->FromDIP(18));
        f.SetPointSize(13);
        f.SetWeight(wxFONTWEIGHT_NORMAL);
        dc.SetFont(f);
        dc.SetTextForeground(wxColour(207, 214, 223));
        dc.DrawText(sub, 0, m_head->FromDIP(62));
        // sekwencja
        const int n = pages(), y = m_head->FromDIP(112), r = m_head->FromDIP(6);
        const int w = sz.x / n;
        f.SetPointSize(9);
        dc.SetFont(f);
        for (int i = 0; i < n; ++i) {
            const int cx = w * i + w / 2;
            if (i + 1 < n) {
                dc.SetPen(wxPen(i < m_page ? kBlue : kLine, 1));
                dc.DrawLine(cx + r, y, cx + w - r, y);
            }
            const bool done = i < m_page, cur = i == m_page;
            if (cur) {
                dc.SetPen(wxPen(wxColour(90, 160, 255, 90), m_head->FromDIP(5)));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawCircle(cx, y, r + 1);
            }
            dc.SetPen(wxPen(done || cur ? kBlue : wxColour(58, 66, 80), 2));
            dc.SetBrush(wxBrush(done ? kBlue : kBg));
            dc.DrawCircle(cx, y, r);
            const wxString t = spaced(seq_name(i), 2);
            dc.SetTextForeground(cur ? kText : done ? kMuted : wxColour(90, 98, 112));
            const wxSize ts = dc.GetTextExtent(t);
            dc.DrawText(t, cx - ts.x / 2, y + r + m_head->FromDIP(10));
        }
    }

    void paint_side()
    {
        wxAutoBufferedPaintDC dc(m_side);
        const wxSize sz = m_side->GetClientSize();
        dc.SetBackground(wxBrush(kBg));
        dc.Clear();
        dc.SetPen(wxPen(kLine, 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        const int pad = m_side->FromDIP(14);
        wxFont f = m_side->GetFont();
        f.SetPointSize(9);
        dc.SetFont(f);
        dc.SetTextForeground(kMuted);
        dc.DrawText(spaced(_L("Your profile"), 2), pad, pad);
        std::vector<std::pair<wxString, wxString>> rows = {{_L("Type"), m_type.empty() ? wxString::FromUTF8("\xE2\x80\x94") : from_u8(m_type)}};
        const wxString names[] = {_L("Nozzle temp."), _L("Flow ratio"), _L("Max flow"), _L("PA"), _L("Retraction")};
        for (int s = 0; s < 5; ++s) rows.push_back({names[s], profile_val(s)});
        int y = pad + m_side->FromDIP(24);
        f.SetPointSize(11);
        dc.SetFont(f);
        for (auto& [k, v] : rows) {
            dc.SetPen(wxPen(wxColour(26, 31, 38), 1));
            dc.DrawLine(pad, y, sz.x - pad, y);
            dc.SetTextForeground(kText);
            dc.DrawText(spaced(k), pad, y + m_side->FromDIP(9));
            const bool empty = v == wxString::FromUTF8("\xE2\x80\x94");
            dc.SetTextForeground(empty ? wxColour(58, 66, 80) : kBlue);
            const wxSize ts = dc.GetTextExtent(v);
            dc.DrawText(v, sz.x - pad - ts.x, y + m_side->FromDIP(9));
            y += m_side->FromDIP(38);
        }
        f.SetPointSize(9);
        dc.SetFont(f);
        dc.SetTextForeground(kMuted);
        dc.DrawText(spaced(m_name, 2), pad, y + m_side->FromDIP(10));
    }

    // Pudełko ✗ BAD / ✓ GOOD: rysunek wzoru i krótki opis.
    wxPanel* example(wxWindow* p, const std::string& svg, bool good, const wxString& text)
    {
        auto* box = new wxPanel(p);
        box->SetBackgroundStyle(wxBG_STYLE_PAINT);
        box->SetMinSize(FromDIP(wxSize(300, 104)));
        wxBitmap bmp = create_scaled_bitmap(svg, this, 70);
        box->Bind(wxEVT_PAINT, [box, bmp, good, text](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(box);
            const wxSize s = box->GetClientSize();
            dc.SetBackground(wxBrush(kBg));
            dc.Clear();
            dc.SetPen(wxPen(good ? kBlue : kLine, 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(0, 0, s.x, s.y);
            const int pad = box->FromDIP(16);
            int x = pad;
            if (bmp.IsOk()) {
                dc.DrawBitmap(bmp, x, (s.y - bmp.GetScaledHeight()) / 2, true);
                x += bmp.GetScaledWidth() + pad;
            }
            wxFont f = box->GetFont();
            f.SetPointSize(15);
            f.SetWeight(wxFONTWEIGHT_BOLD);
            dc.SetFont(f);
            dc.SetTextForeground(good ? kBlue : wxColour(224, 85, 85));
            dc.DrawText(good ? wxString::FromUTF8("\xE2\x9C\x93 ") + spaced(_L("Good")) : wxString::FromUTF8("\xE2\x9C\x97 ") + spaced(_L("Bad")), x, pad);
            f.SetPointSize(12);
            f.SetWeight(wxFONTWEIGHT_NORMAL);
            dc.SetFont(f);
            dc.SetTextForeground(wxColour(207, 214, 223));
            wxString wrapped;
            ::Label::split_lines(dc, s.x - x - pad, text, wrapped);
            dc.DrawText(wrapped, x, pad + box->FromDIP(28));
        });
        return box;
    }

    wxTextCtrl* field(wxWindow* p, wxSizer* s, const wxString& caption, const wxString& value)
    {
        s->Add(label(p, spaced(caption, 2), 9, kMuted), 0, wxTOP, FromDIP(16));
        auto* t = new wxTextCtrl(p, wxID_ANY, value, wxDefaultPosition, FromDIP(wxSize(-1, 34)), wxBORDER_SIMPLE);
        t->SetBackgroundColour(kBg);
        t->SetForegroundColour(kText);
        wxFont f = t->GetFont();
        f.SetPointSize(14);
        t->SetFont(f);
        s->Add(t, 0, wxEXPAND | wxTOP, FromDIP(6));
        return t;
    }

    wxString auto_name() const
    {
        wxString n = m_brand.IsEmpty() ? wxString("Revo") : m_brand;
        if (!m_type.empty()) n += " " + from_u8(m_type);
        if (!m_color_name.IsEmpty()) n += " " + m_color_name;
        return n;
    }

    // Marki z zainstalowanych profili filamentu – do wyboru, ale można wpisać własną.
    static wxArrayString known_brands()
    {
        std::set<std::string> b;
        for (const Preset& p : wxGetApp().preset_bundle->filaments.get_presets()) {
            const std::string v = p.config.opt_string("filament_vendor", 0u);
            if (!v.empty() && v != "Generic" && v.front() != '(') b.insert(v); // bez „(Undefined)”
        }
        wxArrayString out;
        for (const auto& v : b) out.Add(from_u8(v));
        return out;
    }

    void keep_setup()
    {
        if (m_name_ctrl) m_name = m_name_ctrl->GetValue();
        if (m_brand_ctrl) m_brand = m_brand_ctrl->GetValue();
        if (m_color_ctrl) m_color_name = m_color_ctrl->GetValue();
        if (m_color_pick) m_color = m_color_pick->GetColour();
        m_name_ctrl = nullptr;
        m_brand_ctrl = nullptr;
        m_color_ctrl = nullptr;
        m_color_pick = nullptr;
        if (!m_name_edited) m_name = auto_name();
    }

    void show(int page)
    {
        keep_setup();
        m_page = std::max(0, std::min(page, pages() - 1));
        Freeze();
        m_body->DestroyChildren();
        m_next_box->Clear(true);
        auto* s = new wxBoxSizer(wxVERTICAL);
        if (m_page == 0) {
            s->Add(label(m_body, spaced(_L("Material"), 2), 9, kMuted));
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            for (const char* m : {"PLA", "PETG", "ABS", "ASA", "TPU", "PA-CF", "PET-CF", "PA-GF"}) {
                const std::string mat = m;
                row->Add(new PickTile(m_body, mat, m_type == mat, 90, [this, mat] { m_type = mat; show(0); }), 1, wxRIGHT, FromDIP(8));
            }
            s->Add(row, 0, wxEXPAND | wxTOP, FromDIP(6));
            // CF/GF ścierają mosiądz – wymagamy dyszy stalowej (hartowanej)
            if (m_type.find("CF") != std::string::npos || m_type.find("GF") != std::string::npos)
                s->Add(label(m_body, wxString::FromUTF8("\xE2\x9A\xA0 ") + _L("Carbon or glass fibre filament: a hardened steel nozzle is required. Brass wears out within hours."),
                             12, wxColour(255, 196, 92)), 0, wxTOP, FromDIP(10));
            // Marka: własna (wpisz) albo z listy; kolor: nazwa + próbnik – różne kolory potrafią drukować się inaczej
            auto* bc = new wxBoxSizer(wxHORIZONTAL);
            auto* bcol = new wxBoxSizer(wxVERTICAL);
            bcol->Add(label(m_body, spaced(_L("Brand - type your own or pick from the list"), 2), 9, kMuted), 0, wxTOP, FromDIP(16));
            m_brand_ctrl = new wxComboBox(m_body, wxID_ANY, m_brand, wxDefaultPosition, FromDIP(wxSize(-1, 34)), known_brands(), wxCB_DROPDOWN | wxCB_SORT);
            m_brand_ctrl->SetHint(_L("Your own brand"));
            m_brand_ctrl->ChangeValue(m_brand); // nie podstawiaj pierwszej marki z listy
            bcol->Add(m_brand_ctrl, 0, wxEXPAND | wxTOP, FromDIP(6));
            bc->Add(bcol, 1, wxRIGHT, FromDIP(16));
            auto* ccol = new wxBoxSizer(wxVERTICAL);
            ccol->Add(label(m_body, spaced(_L("Colour"), 2), 9, kMuted), 0, wxTOP, FromDIP(16));
            auto* crow = new wxBoxSizer(wxHORIZONTAL);
            m_color_pick = new wxColourPickerCtrl(m_body, wxID_ANY, m_color, wxDefaultPosition, FromDIP(wxSize(44, 34)));
            m_color_ctrl = new wxTextCtrl(m_body, wxID_ANY, m_color_name, wxDefaultPosition, FromDIP(wxSize(-1, 34)), wxBORDER_SIMPLE);
            m_color_ctrl->SetHint(_L("e.g. Orange"));
            m_color_ctrl->SetBackgroundColour(kBg);
            m_color_ctrl->SetForegroundColour(kText);
            crow->Add(m_color_pick, 0, wxRIGHT, FromDIP(8));
            crow->Add(m_color_ctrl, 1);
            ccol->Add(crow, 0, wxEXPAND | wxTOP, FromDIP(6));
            bc->Add(ccol, 1);
            s->Add(bc, 0, wxEXPAND);
            s->Add(label(m_body, _L("Colour matters: pigments change how a filament melts, so each colour gets its own profile."), 11, kMuted), 0, wxTOP, FromDIP(6));
            m_name_ctrl = field(m_body, s, _L("Name on your list"), m_name_edited ? m_name : auto_name());
            auto refresh_name = [this](wxCommandEvent& e) {
                e.Skip();
                if (m_name_edited || !m_name_ctrl) return;
                if (m_brand_ctrl) m_brand = m_brand_ctrl->GetValue();
                if (m_color_ctrl) m_color_name = m_color_ctrl->GetValue();
                m_name_ctrl->ChangeValue(auto_name());
            };
            m_brand_ctrl->Bind(wxEVT_TEXT, refresh_name);
            m_brand_ctrl->Bind(wxEVT_COMBOBOX, refresh_name);
            m_color_ctrl->Bind(wxEVT_TEXT, refresh_name);
            m_name_ctrl->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { e.Skip(); m_name_edited = true; });
            // Suszenie obowiązkowe – nowy nie znaczy suchy
            auto* dry = new wxPanel(m_body);
            dry->SetBackgroundStyle(wxBG_STYLE_PAINT);
            dry->SetMinSize(FromDIP(wxSize(-1, 84)));
            dry->Bind(wxEVT_PAINT, [dry](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(dry);
                const wxSize z = dry->GetClientSize();
                dc.SetBackground(wxBrush(wxColour(11, 22, 38)));
                dc.Clear();
                dc.SetPen(wxPen(kBlue, 1));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(0, 0, z.x, z.y);
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(kBlue));
                dc.DrawRectangle(0, 0, dry->FromDIP(3), z.y);
                wxFont f = dry->GetFont();
                f.SetPointSize(14);
                f.SetWeight(wxFONTWEIGHT_BOLD);
                dc.SetFont(f);
                dc.SetTextForeground(kBlue);
                const int pad = dry->FromDIP(14);
                dc.DrawText(wxString::FromUTF8("\xE2\x9A\xA0 ") + spaced(_L("Dry it first - always")), pad, pad);
                f.SetPointSize(11);
                f.SetWeight(wxFONTWEIGHT_NORMAL);
                dc.SetFont(f);
                dc.SetTextForeground(wxColour(207, 214, 223));
                wxString txt;
                ::Label::split_lines(dc, z.x - 2 * pad, _L("New does not mean dry. Every spool soaks up moisture in the factory, in transport and on the shelf. Wet filament ruins every test - dry it before you start, no exceptions."), txt);
                dc.DrawText(txt, pad, pad + dry->FromDIP(26));
            });
            s->Add(dry, 0, wxEXPAND | wxTOP, FromDIP(18));
            auto* chk = label(m_body, wxString::FromUTF8("\xE2\x98\x90 ") + _L("Filament dried - required, also for a brand new spool") + "\n" +
                                          wxString::FromUTF8("\xE2\x98\x90 ") + _L("Spool loaded in Revo OS: Filament, Load") + "\n" +
                                          wxString::FromUTF8("\xE2\x98\x90 ") + _L("Plate clean, no fingerprints") + "\n" +
                                          wxString::FromUTF8("\xE2\x98\x90 ") + _L("Right nozzle fitted: hardened steel for CF / GF filaments"), 12, wxColour(207, 214, 223));
            s->Add(label(m_body, spaced(_L("Before you start"), 2), 9, kMuted), 0, wxTOP, FromDIP(16));
            s->Add(chk, 0, wxTOP, FromDIP(6));
            m_help->SetLabel(_L("We fill good starting values for you."));
            m_next_box->Add(new FlatButton(this, spaced(_L("Start tests")) + wxString::FromUTF8(" \xE2\x86\x92"), true, [this] { show(1); }));
        } else if (is_step()) {
            const int si = m_page - 1;
            const WizStep& st = m_steps[si];
            auto* top = new wxBoxSizer(wxHORIZONTAL);
            top->Add(label(m_body, m_printed[si] ? _L("1. Test sent - look at the printed result:") : _L("1. First print the test:"), 15, kText), 1, wxALIGN_CENTER_VERTICAL);
            top->Add(new PulseButton(m_body, spaced(_L("Print this test")), !m_printed[si],
                                     [this, si, kind = st.kind] { m_printed[si] = true; if (kind == CalibKind::Temperature || kind == CalibKind::PressureAdvance || kind == CalibKind::MaxVolumetric) g_revo_wizard_material = m_type; wxGetApp().mainframe->run_calibration(kind); }), 0, wxALIGN_CENTER_VERTICAL);
            s->Add(top, 0, wxEXPAND | wxBOTTOM, FromDIP(14));
            auto* ex = new wxBoxSizer(wxHORIZONTAL);
            ex->Add(example(m_body, "revo_wz_" + st.art + "_bad", false, st.bad), 1, wxRIGHT, FromDIP(16));
            ex->Add(example(m_body, "revo_wz_" + st.art + "_good", true, st.good), 1);
            s->Add(ex, 0, wxEXPAND);
            s->Add(label(m_body, "2. " + st.q, 15, kText), 0, wxTOP | wxBOTTOM, FromDIP(16));
            const auto opts = options(si);
            if (m_sel[si] >= int(opts.size())) m_sel[si] = -1;
            const bool towers = si == 0 || si == 2 || si == 4;
            auto* grid = new wxGridSizer(std::max(1, std::min(8, int(opts.size()))), FromDIP(8), FromDIP(8));
            for (size_t i = 0; i < opts.size(); ++i) {
                wxString t = fmt_val(si, opts[i]);
                if (towers && opts.size() > 1 && i == 0) t = _L("Top") + " " + t;
                if (towers && opts.size() > 1 && i + 1 == opts.size()) t = _L("Bottom") + " " + t;
                grid->Add(new PickTile(m_body, t, m_sel[si] == int(i), 70, [this, si, i] { m_sel[si] = int(i); show(m_page); }), 0, wxEXPAND);
            }
            s->Add(grid, 0, wxEXPAND);
            m_help->SetLabel(st.help);
            m_next_box->Add(new FlatButton(this, spaced(_L("Next")) + wxString::FromUTF8(" \xE2\x86\x92"), true, [this] { show(m_page + 1); }));
        } else {
            auto* box = new wxPanel(m_body);
            box->SetBackgroundStyle(wxBG_STYLE_PAINT);
            box->SetMinSize(FromDIP(wxSize(-1, 260)));
            box->Bind(wxEVT_PAINT, [this, box](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(box);
                const wxSize z = box->GetClientSize();
                dc.SetBackground(wxBrush(kBg));
                dc.Clear();
                dc.SetPen(wxPen(kLine, 1));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(0, 0, z.x, z.y);
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(kBlue));
                dc.DrawRectangle(0, 0, box->FromDIP(3), z.y);
                wxFont f = box->GetFont();
                f.SetPointSize(14);
                dc.SetFont(f);
                const wxString names[] = {_L("Nozzle temp."), _L("Flow ratio"), _L("Max flow"), _L("PA"), _L("Retraction")};
                int y = box->FromDIP(14);
                const int pad = box->FromDIP(18);
                for (int s = 0; s < 5; ++s) {
                    dc.SetTextForeground(kText);
                    dc.DrawText(spaced(names[s]), pad, y);
                    // flow: final ratio plus the step picked on the test, so it matches the side panel
                    const wxString v = s == 1 ? wxString::FromDouble(value(1), 3) + wxString::FromUTF8(" \xC2\xB7 ") + profile_val(1) : fmt_val(s, value(s));
                    dc.SetTextForeground(kBlue);
                    const wxSize ts = dc.GetTextExtent(v);
                    dc.DrawText(v, z.x - pad - ts.x, y);
                    y += box->FromDIP(46);
                    dc.SetPen(wxPen(wxColour(26, 31, 38), 1));
                    dc.DrawLine(pad, y - box->FromDIP(10), z.x - pad, y - box->FromDIP(10));
                }
            });
            s->Add(box, 0, wxEXPAND);
            s->Add(label(m_body, wxString::Format(_L("The profile \"%s\" will appear in your filament list. Pick it before you slice - that's it."), m_name),
                         13, wxColour(207, 214, 223)), 0, wxTOP, FromDIP(16));
            m_help->SetLabel(_L("You can run any single test again later from Calibration."));
            m_next_box->Add(new FlatButton(this, spaced(_L("Save profile")), true, [this] { save(); }));
        }
        m_body->SetSizer(s);
        m_body->Layout();
        Layout();
        m_head->Refresh();
        m_side->Refresh();
        Thaw();
    }

    void save()
    {
        const std::string name = into_u8(m_name.Trim().Trim(false));
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
        set_all("nozzle_temperature", value(0));
        set_all("nozzle_temperature_initial_layer", value(0) + 5);
        set_all("filament_flow_ratio", value(1));
        set_all("filament_max_volumetric_speed", value(2));
        set_all("pressure_advance", value(3));
        if (auto* e = cfg.option<ConfigOptionBools>("enable_pressure_advance"))
            for (size_t i = 0; i < e->values.size(); ++i) e->values[i] = true;
        set_all("filament_retraction_length", value(4));
        if (!m_type.empty())
            if (auto* t = cfg.option<ConfigOptionStrings>("filament_type"))
                for (auto& x : t->values) x = m_type;
        if (!m_brand.IsEmpty())
            if (auto* v = cfg.option<ConfigOptionStrings>("filament_vendor"))
                for (auto& x : v->values) x = into_u8(m_brand);
        if (auto* c = cfg.option<ConfigOptionStrings>("filament_colour"))
            for (auto& x : c->values) x = into_u8(m_color.GetAsString(wxC2S_HTML_SYNTAX));
        tab->load_config(cfg);
        tab->save_preset(name);
        MessageDialog(this, wxString::Format(_L("Profile \"%s\" saved. Sometimes evolution. Sometimes [r]evolution."), from_u8(name)),
                      "[r]evo Slicer", wxOK | wxICON_INFORMATION).ShowModal();
        Destroy();
    }

    std::vector<WizStep> m_steps;
    std::vector<int>     m_sel;
    std::vector<bool>    m_printed;
    std::vector<double>  m_cur;
    std::string          m_type;
    wxString             m_brand, m_name;
    int                  m_page = 0;
    wxPanel *            m_head, *m_body, *m_side;
    wxStaticText*        m_help;
    wxBoxSizer*          m_next_box;
    wxTextCtrl *         m_name_ctrl = nullptr;
    wxComboBox*          m_brand_ctrl = nullptr;
    wxTextCtrl*          m_color_ctrl = nullptr;
    wxColourPickerCtrl*  m_color_pick = nullptr;
    wxString             m_color_name;
    wxColour             m_color = wxColour(255, 255, 255);
    bool                 m_name_edited = false;
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
        m_list->SetMinSize(FromDIP(wxSize(1060, 260))); // cały wiersz z DOCK widoczny bez rozciągania okna
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
        // okno rośnie, gdy wiersz (np. długi adres) nie mieści się w całości – DOCK zawsze widoczny
        if (const int extra = m_list_sizer->GetMinSize().x - m_list->GetClientSize().x; extra > 0) {
            SetMinSize(wxSize(GetMinSize().x + extra, GetMinSize().y));
            SetSize(wxSize(GetSize().x + extra, GetSize().y));
            Layout();
        }
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

void note_calib(const Calib_Params& params)
{
    CalibMode m = params.mode;
    if (m == CalibMode::Calib_PA_Pattern || m == CalibMode::Calib_PA_Tower || m == CalibMode::Calib_Auto_PA_Line)
        m = CalibMode::Calib_PA_Line;
    g_calib[int(m)] = params;
}

void note_flow_pass(int pass, bool /*linear*/) { g_flow_pass = pass; }

void show_newbie_wizard(wxWindow* parent)
{
    NewbieWizard dlg(parent);
    dlg.ShowModal();
}

void show_monek_wizard(wxWindow* parent)
{
    // niemodalny – zostaje otwarty, gdy drukujesz testy
    (new NewFilamentWizard(parent))->Show();
}

void show_connect_wizard(wxWindow* parent)
{
    {
        ConnectWizard dlg(parent);
        dlg.ShowModal();
    }
    send_my_printers(); // lista „My printers” na ekranie startowym od razu z nowymi/usuniętymi maszynami
}

// ---------------------------------------------------------------- „My printers” na ekranie startowym
namespace {
struct LiveState
{
    FoundPrinter f;
    bool         online = false;
};
std::map<std::string, LiveState> g_live;      // stan maszyn wg adresu (tylko wątek GUI)
std::atomic<bool>                g_refreshing{false};

// model maszyny z nazwy profilu („Revon Belt 350x350x300 0.4 nozzle @ 192.168.0.38” → „belt”)
std::string model_from_preset(const std::string& n)
{
    if (n.find("Belt") != std::string::npos) return "belt";
    if (n.find("1200x600x500") != std::string::npos) return "1200x600x500";
    if (n.find("600x600x400") != std::string::npos) return "600x600x400";
    return "600x600x600";
}

std::string cover_file(const std::string& m)
{
    return m == "belt" ? "Revon Belt 350x350x300" : "Revon " + m;
}

struct Docked { std::string name, host; };

std::vector<Docked> docked_printers()
{
    std::vector<Docked> out;
    for (const Preset& p : wxGetApp().preset_bundle->printers)
        if (!p.is_system && !p.is_default && p.is_visible)
            if (const auto* h = p.config.option<ConfigOptionString>("print_host"); h && !h->value.empty())
                out.push_back({p.name, h->value});
    return out;
}

// Stan maszyn w tle (agent Revo, bez logowania); po odświeżeniu lista na ekranie startowym jest wysyłana ponownie
void refresh_live()
{
    if (g_refreshing.exchange(true))
        return;
    std::vector<std::string> hosts;
    for (const auto& d : docked_printers())
        hosts.push_back(d.host);
    std::thread([hosts] {
        std::vector<std::pair<std::string, LiveState>> res;
        std::mutex                                     mtx;
        std::vector<std::thread>                       pool;
        for (const auto& h : hosts)
            pool.emplace_back([h, &res, &mtx] {
                LiveState ls;
                Http::get("http://" + h + "/revo/sesja/odkryj")
                    .timeout_connect(2)
                    .timeout_max(4)
                    .on_complete([&ls, &h](std::string b, unsigned st) {
                        try {
                            const auto j = nlohmann::json::parse(b);
                            if (st != 200 || j.value("revo", 0) != 1)
                                return;
                            ls.online     = true;
                            ls.f.ip       = h;
                            ls.f.id       = j.value("machine_id", "");
                            ls.f.model    = j.value("model", "");
                            ls.f.state    = j.value("state", "");
                            ls.f.nozzle   = j.contains("dysza") && j["dysza"].is_number() ? j["dysza"].get<double>() : 0;
                            ls.f.progress = j.contains("postep") && j["postep"].is_number() ? j["postep"].get<int>() : 0;
                        } catch (...) {}
                    })
                    .perform_sync();
                std::lock_guard<std::mutex> l(mtx);
                res.emplace_back(h, ls);
            });
        for (auto& t : pool)
            t.join();
        wxTheApp->CallAfter([res] {
            for (const auto& [h, ls] : res)
                g_live[h] = ls;
            g_refreshing = false;
            send_my_printers(false);
        });
    }).detach();
}
} // namespace

void send_my_printers(bool refresh)
{
    WebViewPanel* home = WebViewPanel::if_built();
    if (!home || !wxGetApp().preset_bundle)
        return;
    const std::string active = wxGetApp().preset_bundle->printers.get_selected_preset_name();
    nlohmann::json    list   = nlohmann::json::array();
    for (const auto& d : docked_printers()) {
        const Preset* p     = wxGetApp().preset_bundle->printers.find_preset(d.name, false);
        std::string   model = model_from_preset(d.name);
        double        noz   = 0;
        if (p)
            if (const auto* n = p->config.option<ConfigOptionFloats>("nozzle_diameter"); n && !n->values.empty())
                noz = n->values.front();
        nlohmann::json e = {{"name", d.name}, {"host", d.host}, {"active", d.name == active}, {"online", false}, {"state", ""}, {"id", ""}};
        if (auto it = g_live.find(d.host); it != g_live.end() && it->second.online) {
            const auto& f = it->second.f;
            if (!f.model.empty())
                model = f.model;
            if (f.nozzle > 0)
                noz = f.nozzle;
            wxString st = state_name(f.state);
            if (f.state == "drukowanie" && f.progress > 0)
                st += wxString::Format(" %d%%", f.progress);
            e["online"] = true;
            e["state"]  = into_u8(st.Upper());
            e["kind"]   = f.state;
            e["id"]     = f.id;
        } else
            e["state"] = into_u8(_L("Offline").Upper());
        e["title"]  = into_u8(model_name(model).Upper());
        e["dims"]   = into_u8(model_dims(model));
        e["nozzle"] = noz > 0 ? wxString::Format("%.1f", noz).ToStdString() : "?";
        e["cover"]  = cover_file(model) + "_cover.png";
        list.push_back(e);
    }
    nlohmann::json msg = {{"command", "revo_printers"}, {"printers", list}};
    home->RunScript(wxString::Format("window.postMessage(%s)", from_u8(msg.dump())));
    if (refresh)
        refresh_live();
}

void remove_my_printer(const std::string& name)
{
    auto&         pr = wxGetApp().preset_bundle->printers;
    const Preset* p  = pr.find_preset(name, false);
    if (!p || p->is_system || p->is_default)
        return;
    // TRN %1% printer name as saved in [r]evo Slicer
    MessageDialog dlg(wxGetApp().mainframe, format_wxstr(_L("Remove %1% from My printers? You can add it again with Find printers."), from_u8(name)),
                      _L("Remove printer"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    if (dlg.ShowModal() != wxID_YES)
        return;
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (pr.get_selected_preset_name() == name && tab) {
        const Preset* parent = pr.get_selected_preset_parent();
        tab->select_preset(parent ? parent->name : pr.default_preset().name);
    }
    pr.delete_preset(name);
    if (tab)
        tab->update_tab_ui();
    wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_PRINTER);
    send_my_printers(false);
}

void select_my_printer(const std::string& name)
{
    auto& pr = wxGetApp().preset_bundle->printers;
    if (const Preset* p = pr.find_preset(name, false); p && name != pr.get_selected_preset_name())
        if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER))
            tab->select_preset(name);
    send_my_printers(false);
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
