#include "RevoMission.hpp"

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "PurgeModeDialog.hpp"
#include "wxExtensions.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/gdicmn.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/webview.h>

namespace Slic3r::GUI::Revo {

namespace {

constexpr const char* PATH_KEY = "revo_mission_path";   // "simple" | "control"
constexpr const char* ASK_KEY  = "revo_mission_ask";     // "0" = don't ask at startup

const char* view_tab(MissionView v)
{
    switch (v) {
    case MissionView::Queue:   return "kol";
    case MissionView::Clients: return "kl";
    case MissionView::Shops:   return "sk";
    default:                   return "zl";
    }
}

// Two cards: Simple Mission and Mission Control.
class StartPathDialog : public DPIDialog
{
public:
    explicit StartPathDialog(wxWindow* parent)
        : DPIDialog(parent, wxID_ANY, _L("[r]evo Mission"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    {
        SetBackgroundColour(*wxWHITE);
        m_control = mission_control();

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* title = new Label(this, Label::Head_16, _L("How do you want to start?"));
        sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

        auto* cards = new wxBoxSizer(wxHORIZONTAL);
        m_simple_card  = new PurgeModeBtnPanel(this, _L("Simple Mission"),
                                               _L("Prepare and print. Just the slicer, as simple as it gets."), "tab_3d_active");
        m_control_card = new PurgeModeBtnPanel(this, _L("Mission Control"),
                                               _L("The slicer plus production: orders, queue and farm, clients, shops and print reports."),
                                               "tab_multi_active");
        for (auto* card : {m_simple_card, m_control_card}) {
            card->SetMinSize(wxSize(FromDIP(220), FromDIP(150)));
            const bool control = card == m_control_card;
            card->Bind(wxEVT_LEFT_DOWN, [this, control](wxMouseEvent&) { select(control); });
            card->Bind(wxEVT_LEFT_DCLICK, [this, control](wxMouseEvent&) { select(control); EndModal(wxID_OK); });
        }
        cards->Add(m_simple_card, 1, wxEXPAND | wxRIGHT, FromDIP(12));
        cards->Add(m_control_card, 1, wxEXPAND);
        sizer->Add(cards, 0, wxEXPAND | wxALL, FromDIP(20));

        auto* ask_row = new wxBoxSizer(wxHORIZONTAL);
        m_dont_ask    = new ::CheckBox(this);
        ask_row->Add(m_dont_ask, 0, wxALIGN_CENTER_VERTICAL);
        ask_row->Add(new Label(this, Label::Body_13, _L("Don't ask at startup (change it any time in the Mission menu)")), 0,
                     wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        sizer->Add(ask_row, 0, wxLEFT | wxRIGHT, FromDIP(20));

        auto* btns = new DialogButtons(this, {"OK"});
        btns->GetOK()->SetLabel(_L("Start"));
        btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        sizer->Add(btns, 0, wxEXPAND);

        select(m_control);
        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    bool control() const { return m_control; }
    bool dont_ask() const { return m_dont_ask->GetValue(); }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        m_dont_ask->Rescale();
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    void select(bool control)
    {
        m_control = control;
        m_simple_card->Select(!control);
        m_control_card->Select(control);
    }

    PurgeModeBtnPanel* m_simple_card{nullptr};
    PurgeModeBtnPanel* m_control_card{nullptr};
    ::CheckBox*        m_dont_ask{nullptr};
    bool               m_control{false};
};

// One window for all Mission views of the docked machine; signs in with the slicer's API key.
class MissionFrame : public DPIFrame
{
public:
    static MissionFrame* s_instance;

    MissionFrame(wxWindow* parent, const wxString& base, const wxString& key)
        : DPIFrame(parent, wxID_ANY, _L("[r]evo Mission"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE), m_base(base)
    {
        SetBackgroundColour(wxColour("#05080c"));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_browser   = WebView::CreateWebView(this, wxEmptyString);
        if (m_browser) {
            sizer->Add(m_browser, 1, wxEXPAND);
            m_browser->Bind(wxEVT_WEBVIEW_LOADED, &MissionFrame::on_loaded, this);
        }
        SetSizer(sizer);
        SetSize(FromDIP(wxSize(1280, 820)));
        SetMinSize(FromDIP(wxSize(900, 600)));
        CenterOnParent();
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
            s_instance = nullptr;
            Destroy();
        });

        // The agent sets the session cookie and redirects to `next`; an older Revo OS ignores `next`
        // and lands on the Revo Panel, which on_loaded() turns into the Mission page.
        if (m_browser)
            WebView::LoadUrl(m_browser, key.IsEmpty() ? m_base + "/mission.html" :
                                                        m_base + "/revo/sesja/klucz?k=" + key + "&next=%2Fmission.html");
    }

    const wxString& base() const { return m_base; }

    void show_view(MissionView view)
    {
        m_tab = view_tab(view);
        if (m_mission_loaded && m_browser)
            WebView::RunScript(m_browser, wxString::Format("if(typeof tab==='function')tab('%s')", m_tab));
        Show();
        Iconize(false);
        Raise();
    }

protected:
    void on_dpi_changed(const wxRect&) override { Refresh(); }

private:
    void on_loaded(wxWebViewEvent& evt)
    {
        evt.Skip();
        if (!m_browser)
            return;
        if (!m_browser->GetCurrentURL().Contains("mission.html")) {
            if (!m_redirected) {
                m_redirected = true;
                WebView::LoadUrl(m_browser, m_base + "/mission.html");
            }
            return;
        }
        m_mission_loaded = true;
        WebView::RunScript(m_browser, wxString::Format("if(typeof tab==='function')tab('%s')", m_tab));
    }

    wxWebView* m_browser{nullptr};
    wxString   m_base;
    wxString   m_tab{"zl"};
    bool       m_redirected{false};
    bool       m_mission_loaded{false};
};

MissionFrame* MissionFrame::s_instance = nullptr;

// Scheme and host of the docked machine's web UI ("http://revon.local").
wxString machine_base(const wxString& url)
{
    const int scheme = url.Find("://");
    const int slash  = url.find('/', scheme == wxNOT_FOUND ? 0 : scheme + 3);
    return slash == wxNOT_FOUND ? url : url.Left(slash);
}

} // namespace

bool mission_control()
{
    AppConfig* c = wxGetApp().app_config;
    return c && c->get(PATH_KEY) == "control";
}

void set_mission_control(bool on)
{
    if (AppConfig* c = wxGetApp().app_config)
        c->set(PATH_KEY, on ? "control" : "simple");
    if (MainFrame* f = wxGetApp().mainframe)
        f->update_mission_menu();
    if (!on && MissionFrame::s_instance)
        MissionFrame::s_instance->Close();
}

void maybe_ask_start_path(wxWindow* parent)
{
    static bool asked = false;
    AppConfig*  c     = wxGetApp().app_config;
    if (asked || !c || c->get(ASK_KEY) == "0")
        return;
    asked = true;
    StartPathDialog dlg(parent);
    if (dlg.ShowModal() != wxID_OK)
        return;
    if (dlg.dont_ask())
        c->set(ASK_KEY, "0");
    set_mission_control(dlg.control());
}

void fill_mission_menu(wxMenu* menu, wxWindow* parent)
{
    if (!menu)
        return;
    while (menu->GetMenuItemCount() > 0)
        menu->Destroy(menu->FindItemByPosition(0));

    if (!mission_control()) {
        append_menu_item(menu, wxID_ANY, _L("Switch to Mission Control"), _L("Orders, queue and farm, clients and shops"),
                         [](wxCommandEvent&) { set_mission_control(true); }, "", nullptr, []() { return true; }, parent);
        return;
    }
    const auto item = [menu, parent](const wxString& name, MissionView view) {
        append_menu_item(menu, wxID_ANY, name, name, [view](wxCommandEvent&) { open_mission(view); }, "", nullptr,
                         []() { return true; }, parent);
    };
    item(_L("Orders"), MissionView::Orders);
    item(_L("Queue and farm"), MissionView::Queue);
    item(_L("Clients"), MissionView::Clients);
    item(_L("Shops and e-mail"), MissionView::Shops);
    menu->AppendSeparator();
    append_menu_item(menu, wxID_ANY, _L("Ask at startup"), _L("Choose Simple Mission or Mission Control when the slicer starts"),
                     [](wxCommandEvent&) {
                         if (AppConfig* c = wxGetApp().app_config)
                             c->set(ASK_KEY, "1");
                     },
                     "", nullptr, []() { return true; }, parent);
    append_menu_item(menu, wxID_ANY, _L("Switch to Simple Mission"), _L("Just the slicer"),
                     [](wxCommandEvent&) { set_mission_control(false); }, "", nullptr, []() { return true; }, parent);
}

void open_mission(MissionView view)
{
    MainFrame* main = wxGetApp().mainframe;
    if (!main)
        return;
    const wxString base = machine_base(main->printer_url());
    if (base.IsEmpty()) {
        InfoDialog(main, _L("[r]evo Mission"),
                   _L("Dock a Revon printer in Main Launch Control first. With a farm, dock the hub machine: it shows the queue of every machine."))
            .ShowModal();
        return;
    }
    // A different machine docked since the window opened: start over with its Mission.
    if (MissionFrame::s_instance && MissionFrame::s_instance->base() != base)
        MissionFrame::s_instance->Close();
    if (!MissionFrame::s_instance)
        MissionFrame::s_instance = new MissionFrame(main, base, main->printer_api_key());
    MissionFrame::s_instance->show_view(view);
}

} // namespace Slic3r::GUI::Revo
