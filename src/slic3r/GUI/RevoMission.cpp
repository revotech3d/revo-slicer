#include "RevoMission.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "WebViewDialog.hpp"
#include "slic3r/Utils/Http.hpp"
#include "wxExtensions.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <string>
#include <thread>

#include <wx/app.h>
#include <wx/colour.h>
#include <wx/time.h>
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

// Start screen state, GUI thread only: the path was picked this session, and the last summary of the docked machine.
bool           g_answered = false;
nlohmann::json g_summary;
std::string    g_summary_base;
long long      g_summary_at = 0;
std::atomic<bool> g_fetching{false};

const char* view_tab(MissionView v)
{
    switch (v) {
    case MissionView::Queue:   return "kol";
    case MissionView::Clients: return "kl";
    case MissionView::Shops:   return "sk";
    default:                   return "zl";
    }
}

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

// Scheme and host of the docked machine's web UI ("http://revon.local"); empty when no printer is docked.
wxString machine_base(wxString url)
{
    if (url.IsEmpty())
        return url;
    if (url.Find("://") == wxNOT_FOUND)
        url = "http://" + url;
    const size_t slash = url.find('/', url.Find("://") + 3);
    return slash == wxString::npos ? url : url.Left(slash);
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
    send_mission_home(on);
}

void send_mission_home(bool refresh)
{
    WebViewPanel* home = WebViewPanel::if_built();
    MainFrame*    main = wxGetApp().mainframe;
    AppConfig*    c    = wxGetApp().app_config;
    if (!home || !main || !c)
        return;
    const bool        control = mission_control();
    const std::string base    = into_u8(machine_base(main->printer_url()));
    if (base != g_summary_base) {
        g_summary      = nullptr;
        g_summary_base = base;
        g_summary_at   = 0;
    }
    nlohmann::json d = {{"control", control}, {"ask", !g_answered && c->get(ASK_KEY) != "0"}, {"summary", g_summary},
                        {"machine", base.empty() ? "" : base.substr(base.find("://") + 3)}};
    home->RunScript(wxString::Format("window.postMessage(%s)", from_u8(nlohmann::json({{"command", "revo_mission"}, {"data", d}}).dump())));

    // Every 20 s while the start screen polls; the hub answers for the whole farm.
    const long long now = wxGetUTCTime();
    if (!refresh || !control || base.empty() || now - g_summary_at < 20 || g_fetching.exchange(true))
        return;
    g_summary_at = now;
    std::thread([base, key = into_u8(main->printer_api_key())] {
        nlohmann::json res;
        Http::get(base + "/revo/sesja/mission-podsumowanie")
            .header("X-Api-Key", key)
            .timeout_connect(2)
            .timeout_max(8)
            .on_complete([&res](std::string body, unsigned status) {
                try {
                    if (status == 200)
                        res = nlohmann::json::parse(body);
                } catch (...) {}
            })
            .perform_sync();
        wxTheApp->CallAfter([base, res] {
            g_fetching = false;
            if (base == g_summary_base && res.is_object()) {
                g_summary = res;
                send_mission_home(false);
            }
        });
    }).detach();
}

void handle_home_message(const std::string& action, bool control, bool remember, const std::string& view)
{
    if (action == "start") {
        g_answered = true;
        if (AppConfig* c = wxGetApp().app_config; c && remember)
            c->set(ASK_KEY, "0");
        set_mission_control(control);
    } else if (action == "switch") {
        set_mission_control(!mission_control());
    } else if (action == "open") {
        static const std::map<std::string, MissionView> views = {
            {"orders", MissionView::Orders}, {"queue", MissionView::Queue}, {"clients", MissionView::Clients}, {"shops", MissionView::Shops}};
        const auto it = views.find(view);
        open_mission(it == views.end() ? MissionView::Orders : it->second);
    } else
        send_mission_home(true);
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
                         g_answered = false;
                         send_mission_home(false);
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
