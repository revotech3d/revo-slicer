#include "RevoMission.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "RevoKreator.hpp"
#include "WebViewDialog.hpp"
#include "slic3r/Utils/Http.hpp"
#include "wxExtensions.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <wx/app.h>
#include <wx/colour.h>
#include <wx/datetime.h>
#include <wx/event.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/webview.h>

namespace fs = boost::filesystem;

namespace Slic3r::GUI::Revo {

namespace {

constexpr const char* PATH_KEY = "revo_mission_path";   // "simple" | "control"
constexpr const char* ASK_KEY  = "revo_mission_ask";     // "0" = don't ask at startup

bool g_answered = false;   // the start path was picked this session

const char* view_id(MissionView v)
{
    switch (v) {
    case MissionView::Assigned:   return "as";
    case MissionView::Completed:  return "ok";
    case MissionView::Prints:     return "wy";
    case MissionView::Printers:   return "pr";
    case MissionView::Clients:    return "kl";
    default:                      return "un";
    }
}

// ---------- local store: orders, clients and finished prints live on this computer ----------

fs::path store_dir() { return fs::path(data_dir()) / "revo_mission"; }
fs::path store_file() { return store_dir() / "mission.json"; }

std::string read_store()
{
    boost::nowide::ifstream f(store_file().string(), std::ios::binary);
    if (!f)
        return "null";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    try {
        return nlohmann::json::parse(s).dump();
    } catch (...) {
        return "null";
    }
}

// Written to a temporary file and renamed, so a crash never leaves half a file. One copy per day is kept for 30 days.
bool write_store(const nlohmann::json& data)
{
    try {
        fs::create_directories(store_dir());
        const fs::path tmp = store_file().string() + ".tmp";
        {
            boost::nowide::ofstream f(tmp.string(), std::ios::binary | std::ios::trunc);
            f << data.dump(1);
            if (!f)
                return false;
        }
        fs::rename(tmp, store_file());
        const fs::path daily = store_dir() / ("mission-" + into_u8(wxDateTime::Now().FormatISODate()) + ".json");
        fs::copy_file(store_file(), daily, fs::copy_options::overwrite_existing);
        std::vector<fs::path> old;
        for (const auto& e : fs::directory_iterator(store_dir()))
            if (e.path().filename().string().rfind("mission-", 0) == 0)
                old.push_back(e.path());
        std::sort(old.begin(), old.end());
        for (size_t i = 0; i + 30 < old.size(); ++i)
            fs::remove(old[i]);
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json g_summary;   // last summary the Mission page saved, for the start screen

// Scheme and host of a printer's web UI ("http://192.168.0.38").
std::string machine_base(std::string url)
{
    if (url.empty())
        return url;
    if (url.find("://") == std::string::npos)
        url = "http://" + url;
    const size_t slash = url.find('/', url.find("://") + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}

// The MISSION tab: the local Mission Control page.
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
            m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &MissionPanel::on_message, this);
            m_browser->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent& e) {
                e.Skip();
                m_ready = true;
                if (!m_view.empty())
                    show(m_view);
            });
            CallAfter([this] {
                WebView::LoadUrl(m_browser, file_url_from_path(fs::path(resources_dir()) / "web" / "mission" / "index.html"));
            });
        }
        SetSizer(sizer);
    }
    ~MissionPanel() override { s_instance = nullptr; }

    void show(const std::string& view)
    {
        m_view = view;
        if (m_ready)
            call("MissionApp.show", nlohmann::json(view).dump());
    }

    // New printer states: the Printers view and the printer choice in orders follow.
    void printers_changed()
    {
        if (m_ready)
            call("MissionApp.onPrinters", my_printers_json().dump());
    }

private:
    void call(const char* fn, const std::string& args) { WebView::RunScript(m_browser, wxString::Format("%s(%s)", fn, from_u8(args))); }

    void on_message(wxWebViewEvent& evt)
    {
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(into_u8(evt.GetString()));
        } catch (...) {
            return;
        }
        const std::string cmd = msg.value("cmd", "");
        // Replies run after this handler returns: no window work on the stack of a script message.
        if (cmd == "load") {
            CallAfter([this] { call("MissionApp.onLoad", read_store()); });
        } else if (cmd == "save" && msg.contains("data")) {
            const bool ok = write_store(msg["data"]);
            g_summary     = msg["data"].value("podsumowanie", nlohmann::json());
            CallAfter([this, ok] {
                call("MissionApp.onSaved", ok ? "true" : "false");
                send_mission_home(false);
            });
        } else if (cmd == "printers") {
            CallAfter([this] {
                printers_changed();
                const long long now = wxGetUTCTime();
                if (now - m_refreshed > 15) {
                    m_refreshed = now;
                    refresh_printers();
                }
            });
        } else if (cmd == "prints") {
            fetch_prints(msg.value("since", nlohmann::json::object()));
        }
    }

    // Finished prints from every added printer that answers; offline printers are asked again later.
    void fetch_prints(const nlohmann::json& since)
    {
        for (const PrinterLink& p : printer_links()) {
            if (p.key.empty())
                continue;
            const std::string base = machine_base(p.host);
            const std::string od   = since.is_object() && since.contains(base.substr(base.find("://") + 3)) ?
                                         since.value(base.substr(base.find("://") + 3), "") : "";
            std::thread([p, base, od] {
                std::string body;
                Http::get(base + "/revo/sesja/wydruki?od=" + Http::url_encode(od))
                    .header("X-Api-Key", p.key)
                    .timeout_connect(2)
                    .timeout_max(8)
                    .on_complete([&body](std::string b, unsigned status) {
                        if (status == 200)
                            body = std::move(b);
                    })
                    .perform_sync();
                if (body.empty())
                    return;
                wxTheApp->CallAfter([p, base, body] {
                    if (!s_instance)
                        return;
                    try {
                        const auto j = nlohmann::json::parse(body);
                        const std::string host = base.substr(base.find("://") + 3);
                        s_instance->call("MissionApp.onPrints", nlohmann::json(host).dump() + "," + nlohmann::json(p.name).dump() + "," +
                                                                    j.value("wydruki", nlohmann::json::array()).dump());
                    } catch (...) {}
                });
            }).detach();
        }
    }

    wxWebView*  m_browser{nullptr};
    bool        m_ready{false};
    std::string m_view;
    long long   m_refreshed{0};
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
    send_mission_home(false);
}

void send_mission_home(bool /*refresh*/)
{
    WebViewPanel* home = WebViewPanel::if_built();
    AppConfig*    c    = wxGetApp().app_config;
    if (!home || !c)
        return;
    if (g_summary.is_null())
        try {
            g_summary = nlohmann::json::parse(read_store()).value("podsumowanie", nlohmann::json());
        } catch (...) {}
    nlohmann::json d = {{"control", mission_control()}, {"ask", !g_answered && c->get(ASK_KEY) != "0"}, {"summary", g_summary}};
    home->RunScript(wxString::Format("window.postMessage(%s)", from_u8(nlohmann::json({{"command", "revo_mission"}, {"data", d}}).dump())));
}

void handle_home_message(const std::string& action, bool control, bool remember, const std::string& view)
{
    if (action == "start") {
        g_answered = true;
        if (AppConfig* c = wxGetApp().app_config; c && remember)
            c->set(ASK_KEY, "0");
        set_mission_control(control);
        if (control)
            open_mission(MissionView::Unassigned);
    } else if (action == "switch") {
        set_mission_control(!mission_control());
    } else if (action == "open") {
        static const std::map<std::string, MissionView> views = {{"un", MissionView::Unassigned}, {"as", MissionView::Assigned},
                                                                 {"ok", MissionView::Completed}, {"wy", MissionView::Prints},
                                                                 {"pr", MissionView::Printers},  {"kl", MissionView::Clients}};
        const auto it = views.find(view);
        open_mission(it == views.end() ? MissionView::Unassigned : it->second);
    } else
        send_mission_home(false);
}

void fill_mission_menu(wxMenu* menu, wxWindow* parent)
{
    if (!menu)
        return;
    while (menu->GetMenuItemCount() > 0)
        menu->Destroy(menu->FindItemByPosition(0));

    if (!mission_control()) {
        append_menu_item(menu, wxID_ANY, _L("Switch to Mission Control"), _L("Orders, printers, clients and finished prints"),
                         [](wxCommandEvent&) { set_mission_control(true); }, "", nullptr, []() { return true; }, parent);
        return;
    }
    const auto item = [menu, parent](const wxString& name, MissionView view) {
        append_menu_item(menu, wxID_ANY, name, name, [view](wxCommandEvent&) { open_mission(view); }, "", nullptr,
                         []() { return true; }, parent);
    };
    item(_L("Unassigned orders"), MissionView::Unassigned);
    item(_L("Assigned orders"), MissionView::Assigned);
    item(_L("Completed orders"), MissionView::Completed);
    item(_L("Unassigned prints"), MissionView::Prints);
    item(_L("Printers and queues"), MissionView::Printers);
    item(_L("Clients"), MissionView::Clients);
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

void mission_printer_changed()
{
    if (MissionPanel::s_instance)
        MissionPanel::s_instance->printers_changed();
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
    if (!mission_control())
        set_mission_control(true);
    main->select_tab(wxString(TAB_ID_MISSION));
    if (MissionPanel::s_instance)
        MissionPanel::s_instance->show(view_id(view));
}

} // namespace Slic3r::GUI::Revo
