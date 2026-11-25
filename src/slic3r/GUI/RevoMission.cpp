#include "RevoMission.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "MainFrame.hpp"
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

// The Mission tab: the docked machine's Mission page, signed in with the slicer's API key.
class MissionPanel : public wxPanel
{
public:
    static MissionPanel* s_instance;

    explicit MissionPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(wxColour("#05080c"));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_browser   = WebView::CreateWebView(this, wxEmptyString);
        if (m_browser) {
            sizer->Add(m_browser, 1, wxEXPAND);
            m_browser->Bind(wxEVT_WEBVIEW_LOADED, &MissionPanel::on_loaded, this);
            m_browser->Bind(wxEVT_WEBVIEW_ERROR, [this](wxWebViewEvent& e) {
                e.Skip();
                // TRN %1% is the machine address, e.g. http://192.168.0.38
                message(format_wxstr(_L("Can't reach %1%. Check that the machine is on and in the same network."), m_base));
                m_base.clear();   // retry on the next visit
            });
            message(_L("Dock a Revon printer in Main Launch Control to see orders, queue and farm."));
        }
        SetSizer(sizer);
        // Opened from the tab bar: follow the machine docked now.
        Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
            e.Skip();
            if (!e.IsShown())
                return;
            if (MainFrame* main = wxGetApp().mainframe) {
                const wxString base = machine_base(main->printer_url());
                if (!base.IsEmpty() && base != m_base)
                    show(base, main->printer_api_key(), MissionView::Orders);
            }
        });
    }
    ~MissionPanel() override { s_instance = nullptr; }

    // Loads the machine's Mission page when the docked machine changed, then shows `view`.
    void show(const wxString& base, const wxString& key, MissionView view)
    {
        m_tab = view_tab(view);
        if (!m_browser)
            return;
        if (base != m_base) {
            message(format_wxstr(_L("Connecting to %1%…"), base));
            m_base           = base;
            m_redirected     = false;
            m_mission_loaded = false;
            // The agent sets the session cookie and redirects to `next`; an older Revo OS ignores `next`
            // and lands on the Revo Panel, which on_loaded() turns into the Mission page.
            WebView::LoadUrl(m_browser, key.IsEmpty() ? m_base + "/mission.html" :
                                                        m_base + "/revo/sesja/klucz?k=" + key + "&next=%2Fmission.html");
        } else if (m_mission_loaded)
            WebView::RunScript(m_browser, wxString::Format("if(typeof tab==='function')tab('%s')", m_tab));
    }

private:
    // A dark page in the slicer's style instead of a blank white one.
    void message(const wxString& text)
    {
        wxString t = text;
        t.Replace("&", "&amp;");
        t.Replace("<", "&lt;");
        m_browser->SetPage("<html><body style=\"margin:0;background:#05080c;color:#9aa4b2;font:12px -apple-system,Segoe UI,sans-serif;"
                           "letter-spacing:3px;text-transform:uppercase;display:flex;align-items:center;justify-content:center;height:100vh\">"
                           "<div style=\"text-align:center\"><div style=\"color:#5aa0ff;font-size:11px;margin-bottom:14px\">[R]EVO MISSION CONTROL</div>" +
                               t + "</div></body></html>",
                           "about:blank");
    }

    void on_loaded(wxWebViewEvent& evt)
    {
        evt.Skip();
        if (m_base.IsEmpty() || m_browser->GetCurrentURL().StartsWith("about:"))
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

MissionPanel* MissionPanel::s_instance = nullptr;

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
        if (control)
            if (MainFrame* main = wxGetApp().mainframe)
                main->select_tab(wxString(TAB_ID_MISSION));
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

wxWindow* create_mission_page(wxWindow* parent)
{
    if (!MissionPanel::s_instance)
        MissionPanel::s_instance = new MissionPanel(parent);
    return MissionPanel::s_instance;
}

void open_mission(MissionView view)
{
    MainFrame* main = wxGetApp().mainframe;
    if (!main)
        return;
    const wxString base = machine_base(main->printer_url());
    if (!mission_control())
        set_mission_control(true);
    main->select_tab(wxString(TAB_ID_MISSION));
    // Without a docked machine the tab itself says what to do.
    if (MissionPanel::s_instance && !base.IsEmpty())
        MissionPanel::s_instance->show(base, main->printer_api_key(), view);
}

} // namespace Slic3r::GUI::Revo
