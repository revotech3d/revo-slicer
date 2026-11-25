#include "RevoMission.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "RevoKreator.hpp"
#include "WebViewDialog.hpp"
#include "Plater.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/RevoMissionNet.hpp"
#include "wxExtensions.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wx/app.h>
#include <wx/colour.h>
#include <wx/datetime.h>
#include <wx/event.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/utils.h>
#include <wx/webview.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

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

constexpr const char* DEMO_KEY = "revo_presentation";   // Preferences → Presentation mode
std::atomic<bool>     g_demo{false};                      // read by the station server thread too

// Presentation mode keeps its sample orders in a folder of its own, so the user's orders stay as they are.
fs::path store_dir() { return fs::path(data_dir()) / (g_demo ? "revo_mission_demo" : "revo_mission"); }
fs::path store_file() { return store_dir() / "mission.json"; }
fs::path cache_file() { return store_dir() / "station-cache.json"; }
fs::path station_file() { return store_dir() / "station.json"; }

std::mutex             g_store_mx;   // the page and the station server both write the store
std::atomic<long long> g_rev{1};     // bumped on every write; other stations ask for changes since a revision

nlohmann::json read_json(const fs::path& file)
{
    boost::nowide::ifstream f(file.string(), std::ios::binary);
    if (!f)
        return nullptr;
    try {
        return nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
    } catch (...) {
        return nullptr;
    }
}

bool write_json(const fs::path& file, const nlohmann::json& data)
{
    try {
        fs::create_directories(file.parent_path());
        const fs::path tmp = file.string() + ".tmp";
        {
            boost::nowide::ofstream f(tmp.string(), std::ios::binary | std::ios::trunc);
            f << data.dump(1);
            if (!f)
                return false;
        }
        fs::rename(tmp, file);
        return true;
    } catch (...) {
        return false;
    }
}

std::string today_iso() { return into_u8(wxDateTime::Now().FormatISODate()); }

// Written to a temporary file and renamed, so a crash never leaves half a file. One copy per day is kept for 30 days.
bool write_store(const nlohmann::json& data)
{
    if (!write_json(store_file(), data))
        return false;
    try {
        const fs::path daily = store_dir() / ("mission-" + today_iso() + ".json");
        fs::copy_file(store_file(), daily, fs::copy_options::overwrite_existing);
        std::vector<fs::path> old;
        for (const auto& e : fs::directory_iterator(store_dir()))
            if (e.path().filename().string().rfind("mission-", 0) == 0)
                old.push_back(e.path());
        std::sort(old.begin(), old.end());
        for (size_t i = 0; i + 30 < old.size(); ++i)
            fs::remove(old[i]);
    } catch (...) {}
    return true;
}

// The incoming copy merged into the stored one (newer changes win, see RevoNet::merge_store). Any thread.
std::pair<nlohmann::json, long long> save_merged(const nlohmann::json& incoming)
{
    std::lock_guard<std::mutex> lock(g_store_mx);
    nlohmann::json merged = RevoNet::merge_store(read_json(store_file()), incoming);
    write_store(merged);
    return {merged, ++g_rev};
}

std::pair<nlohmann::json, long long> load_local()
{
    std::lock_guard<std::mutex> lock(g_store_mx);
    return {read_json(store_file()), g_rev.load()};
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

// ---------- the station: this computer as the main station, or working with another one ----------

struct Station
{
    bool           share{false};                 // serve the orders on the local network
    std::string    name, id, salt, key;          // hex; this computer as the main station
    std::string    host, jkey, jname, jid;       // the main station this computer works with (host empty: none)
    int            port{RevoNet::HTTP_PORT};
    nlohmann::json prof = {{"machine", true}, {"process", true}, {"filament", true}};
    std::string    last_sync;
};
Station g_st;
bool    g_st_loaded = false;

void load_station()
{
    if (g_st_loaded)
        return;
    g_st_loaded        = true;
    const nlohmann::json j = read_json(station_file());
    if (j.is_object()) {
        g_st.share     = j.value("share", false);
        g_st.name      = j.value("name", "");
        g_st.id        = j.value("id", "");
        g_st.salt      = j.value("salt", "");
        g_st.key       = j.value("key", "");
        g_st.host      = j.value("host", "");
        g_st.port      = j.value("port", int(RevoNet::HTTP_PORT));
        g_st.jkey      = j.value("jkey", "");
        g_st.jname     = j.value("jname", "");
        g_st.jid       = j.value("jid", "");
        g_st.last_sync = j.value("last_sync", "");
        if (j.contains("prof") && j["prof"].is_object())
            g_st.prof.update(j["prof"]);
    }
    if (g_st.id.empty())
        g_st.id = RevoNet::to_hex(RevoNet::random_bytes(8));
    if (g_st.name.empty())
        g_st.name = into_u8(wxGetHostName());
}

void save_station()
{
    write_json(station_file(), {{"share", g_st.share}, {"name", g_st.name}, {"id", g_st.id}, {"salt", g_st.salt}, {"key", g_st.key},
                                {"host", g_st.host}, {"port", g_st.port}, {"jkey", g_st.jkey}, {"jname", g_st.jname}, {"jid", g_st.jid},
                                {"prof", g_st.prof}, {"last_sync", g_st.last_sync}});
}

bool client() { return !g_st.host.empty(); }

bool           g_connected = false;   // client: the main station answered last time
std::string    g_server_error;
std::unique_ptr<RevoNet::Server> g_server;

// Folder of this computer's user presets (data_dir()/user/<folder>), as the preset bundle uses it.
std::string user_dir()
{
    std::string folder = wxGetApp().app_config ? wxGetApp().app_config->get("preset_folder") : "";
    if (folder.empty())
        folder = DEFAULT_USER_FOLDER_NAME;
    return (fs::path(data_dir()) / PRESET_USER_DIR / folder).string();
}
std::string backup_dir() { return (store_dir() / "profile-backup" / today_iso()).string(); }

std::vector<std::string> synced_kinds()
{
    std::vector<std::string> kinds;
    for (const char* k : {"machine", "process", "filament"})
        if (g_st.prof.value(k, true))
            kinds.emplace_back(k);
    return kinds;
}

std::string friendly(const std::string& err)
{
    if (err == "auth")
        return "Wrong password";
    if (err == "clock")
        return "The clocks of the two computers differ by more than 5 minutes";
    return "Main station not reachable (" + err + ")";
}

nlohmann::json net_json()
{
    nlohmann::json j = {{"role", client() ? "client" : g_server && g_server->running() ? "main" : "local"},
                        {"share", g_st.share},
                        {"name", g_st.name},
                        {"port", RevoNet::HTTP_PORT},
                        {"addrs", RevoNet::local_addresses()},
                        {"error", g_server_error},
                        {"has_password", !g_st.key.empty()},
                        {"prof", g_st.prof},
                        {"last_sync", g_st.last_sync}};
    if (client()) {
        const nlohmann::json cache = read_json(cache_file());
        j["station"] = {{"name", g_st.jname}, {"host", g_st.host}, {"port", g_st.port}, {"connected", g_connected},
                        {"cache_at", cache.is_object() ? cache.value("at", "") : ""}};
    }
    return j;
}

// Profiles another station sent: new ones are loaded now, changed ones when the slicer starts next time.
void load_new_profiles()
{
    GUI_App& app = wxGetApp();
    if (!app.preset_bundle || !app.app_config || !app.mainframe || app.is_closing())
        return;
    std::string folder = app.app_config->get("preset_folder");
    app.preset_bundle->load_user_presets(folder.empty() ? std::string(DEFAULT_USER_FOLDER_NAME) : folder,
                                         ForwardCompatibilitySubstitutionRule::Enable);
    app.mainframe->update_side_preset_ui();
    if (app.plater())
        app.plater()->sidebar().update_all_preset_comboboxes();
}

void deliver(const nlohmann::json& store, long long rev);

// Requests from other stations, on the server thread.
nlohmann::json serve(const nlohmann::json& r, const std::string& udir, const std::string& bdir)
{
    const std::string cmd = r.value("cmd", "");
    if (cmd == "ping")
        return {{"ok", true}};
    if (cmd == "load") {
        auto [store, rev] = load_local();
        if (r.value("rev", -1LL) == rev)
            return {{"rev", rev}};
        return {{"rev", rev}, {"store", store}};
    }
    if (cmd == "save") {
        const std::pair<nlohmann::json, long long> saved = save_merged(r.at("data"));
        wxTheApp->CallAfter([saved] { deliver(saved.first, saved.second); });
        return {{"rev", saved.second}, {"store", saved.first}};
    }
    if (cmd == "plist")
        return {{"list", RevoNet::list_profiles(udir, {"machine", "process", "filament"})}};
    if (cmd == "pget") {
        std::string data;
        int64_t     mtime = 0;
        if (!RevoNet::read_profile(udir, r.value("p", ""), data, mtime))
            throw std::runtime_error("no such profile");
        return {{"d", data}, {"m", mtime}};
    }
    if (cmd == "pput") {
        if (!RevoNet::write_profile(udir, bdir, r.value("p", ""), r.value("d", ""), r.value("m", int64_t(0))))
            throw std::runtime_error("profile not written");
        wxTheApp->CallAfter([] { load_new_profiles(); });
        return {{"ok", true}};
    }
    throw std::runtime_error("unknown command");
}

bool start_server()
{
    g_server_error.clear();
    if (!g_server)
        g_server = std::make_unique<RevoNet::Server>();
    const std::string udir = user_dir(), bdir = backup_dir();
    return g_server->start(RevoNet::from_hex(g_st.key), RevoNet::from_hex(g_st.salt), g_st.name, g_st.id,
                           [udir, bdir](const nlohmann::json& r) { return serve(r, udir, bdir); }, g_server_error);
}

#ifdef _WIN32
// Windows Defender Firewall drops connections to a new listening port until a rule allows them, and the "allow access"
// prompt it shows for the app is easy to dismiss. Sharing therefore adds its own rule for the station ports, limited to
// the local subnet, once; adding it needs the administrator (UAC).
constexpr const wchar_t* FIREWALL_RULE = L"[r]evo Mission";

DWORD run_hidden(const std::wstring& exe, const std::wstring& args, bool elevated)
{
    SHELLEXECUTEINFOW info{};
    info.cbSize       = sizeof(info);
    info.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    info.lpVerb       = elevated ? L"runas" : L"open";
    info.lpFile       = exe.c_str();
    info.lpParameters = args.c_str();
    info.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess)
        return DWORD(-1);
    WaitForSingleObject(info.hProcess, 60000);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code;
}

// Worker thread. False when the rule is missing and the user declined the administrator prompt.
bool ensure_firewall_rule()
{
    const std::wstring name = std::wstring(L"name=\"") + FIREWALL_RULE + L"\"";
    if (run_hidden(L"netsh.exe", L"advfirewall firewall show rule " + name, false) == 0)
        return true;
    const std::wstring common = L"advfirewall firewall add rule " + name + L" dir=in action=allow remoteip=localsubnet profile=any";
    const std::wstring args   = L"/c netsh " + common + L" protocol=TCP localport=" + std::to_wstring(RevoNet::HTTP_PORT) + L" && netsh " +
                              common + L" protocol=UDP localport=" + std::to_wstring(RevoNet::DISCOVERY_PORT);
    return run_hidden(L"cmd.exe", args, true) == 0;
}
#endif

void stop_server()
{
    if (g_server)
        g_server->stop();
}

// A request to the main station on a worker thread; done(ok, reply, error) runs on the main thread.
void ask_station(nlohmann::json req, std::function<void(bool, const nlohmann::json&, const std::string&)> done, int timeout_s = 10)
{
    const std::string host = g_st.host, key = RevoNet::from_hex(g_st.jkey);
    const int         port = g_st.port;
    std::thread([host, port, key, req = std::move(req), done = std::move(done), timeout_s] {
        nlohmann::json reply;
        std::string    err;
        const bool     ok = RevoNet::request(host, static_cast<unsigned short>(port), key, req, reply, err, timeout_s);
        wxTheApp->CallAfter([ok, reply, err, done] {
            if (!wxGetApp().is_closing())
                done(ok, reply, err);
        });
    }).detach();
}

// The MISSION tab: the local Mission Control page.
class MissionPanel : public wxPanel
{
public:
    static MissionPanel* s_instance;

    explicit MissionPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
    {
        load_station();
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
                wxString url = file_url_from_path(fs::path(resources_dir()) / "web" / "mission" / "index.html");
                const wxString lang = wxGetApp().current_language_code_safe();
                if (!lang.empty())
                    url += "?lang=" + lang;
                WebView::LoadUrl(m_browser, url);
            });
        }
        SetSizer(sizer);
    }
    ~MissionPanel() override
    {
        if (s_instance == this)
            s_instance = nullptr;
    }

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

    // A newer copy of the orders (saved here, by another station, or fetched from the main station).
    void merged(const nlohmann::json& store, long long rev)
    {
        if (rev < m_rev)
            return;
        m_rev = rev;
        call("MissionApp.onMerged", store.dump());
    }

    void net_changed() { call("MissionApp.onNet", net_json().dump()); }

    // Loads the orders from wherever this computer works with them: its own store, or the main station.
    void load()
    {
        if (!client()) {
            auto [store, rev] = load_local();
            m_rev             = rev;
            call("MissionApp.onLoad", store.dump() + "," + net_json().dump());
            return;
        }
        ask_station({{"cmd", "load"}}, [](bool ok, const nlohmann::json& reply, const std::string& err) {
            if (!s_instance)
                return;
            g_connected = ok;
            if (ok)
                s_instance->took(reply);
            else
                s_instance->fall_back(err);
        });
    }

private:
    void call(const char* fn, const std::string& args) { WebView::RunScript(m_browser, wxString::Format("%s(%s)", fn, from_u8(args))); }

    // The main station's copy: kept as the read-only copy for when it is off.
    void took(const nlohmann::json& reply)
    {
        const long long rev = reply.value("rev", 0LL);
        if (!reply.contains("store"))
            return;
        const nlohmann::json& store = reply["store"];
        write_json(cache_file(), {{"station", g_st.jid}, {"at", into_u8(wxDateTime::Now().Format("%Y-%m-%d %H:%M"))}, {"store", store}});
        g_summary = store.value("podsumowanie", nlohmann::json());
        send_mission_home(false);
        m_rev = rev;
        call("MissionApp.onLoad", store.dump() + "," + net_json().dump());
    }

    // The main station did not answer: the last copy, read-only.
    void fall_back(const std::string& err)
    {
        const nlohmann::json cache = read_json(cache_file());
        m_rev                      = -1;
        nlohmann::json net         = net_json();
        net["error"]               = friendly(err);
        call("MissionApp.onLoad", (cache.is_object() ? cache.value("store", nlohmann::json()) : nlohmann::json()).dump() + "," + net.dump());
    }

    void save(const nlohmann::json& data)
    {
        if (!client()) {
            auto [store, rev] = save_merged(data);
            deliver(store, rev);
            return;
        }
        if (!g_connected) {
            call("MissionApp.onSaveFailed", nlohmann::json("The main station is off – this copy is read-only").dump());
            load();
            return;
        }
        ask_station({{"cmd", "save"}, {"data", data}}, [](bool ok, const nlohmann::json& reply, const std::string& err) {
            if (!s_instance)
                return;
            if (ok) {
                write_json(cache_file(), {{"station", g_st.jid}, {"at", into_u8(wxDateTime::Now().Format("%Y-%m-%d %H:%M"))},
                                          {"store", reply["store"]}});
                deliver(reply["store"], reply.value("rev", 0LL));
                return;
            }
            g_connected = false;
            s_instance->call("MissionApp.onSaveFailed", nlohmann::json(friendly(err) + " – the change was not saved").dump());
            s_instance->fall_back(err);
        });
    }

    // Client: changes other stations made since the revision shown.
    void poll()
    {
        if (!client())
            return;
        const bool was = g_connected;
        ask_station({{"cmd", "load"}, {"rev", was ? m_rev : -1LL}},
                    [was](bool ok, const nlohmann::json& reply, const std::string&) {
                        if (!s_instance)
                            return;
                        g_connected = ok;
                        if (ok && !was)
                            s_instance->took(reply);
                        else if (ok && reply.contains("store")) {
                            write_json(cache_file(), {{"station", g_st.jid}, {"at", into_u8(wxDateTime::Now().Format("%Y-%m-%d %H:%M"))},
                                                      {"store", reply["store"]}});
                            deliver(reply["store"], reply.value("rev", 0LL));
                        } else if (was != ok)
                            s_instance->net_changed();
                    },
                    5);
    }

    void result(bool ok, const std::string& msg) { call("MissionApp.onNetResult", (ok ? "true," : "false,") + nlohmann::json(msg).dump()); }

    // Sharing on the local network: on/off, station name and password.
    void share(bool on, const std::string& name, const std::string& password)
    {
        if (on && client())
            return result(false, "Disconnect from the main station first");
        if (on && password.empty() && g_st.key.empty())
            return result(false, "Set a password for the station first");
        if (!name.empty())
            g_st.name = name;
        const auto apply = [on](const std::string& salt, const std::string& key) {
            if (!s_instance)
                return;
            if (!key.empty()) {
                g_st.salt = RevoNet::to_hex(salt);
                g_st.key  = RevoNet::to_hex(key);
            }
            g_st.share = on;
            stop_server();
            bool ok = true;
            if (on && !(ok = start_server()))
                g_st.share = false;
            save_station();
            s_instance->result(ok, ok ? (on ? "Sharing on the local network" : "Sharing off – orders stay on this computer") :
                                        "Could not open port " + std::to_string(RevoNet::HTTP_PORT) + ": " + g_server_error);
            s_instance->net_changed();
#ifdef _WIN32
            if (ok && on)
                std::thread([] {
                    if (!ensure_firewall_rule())
                        wxTheApp->CallAfter([] {
                            if (s_instance)
                                s_instance->result(false, "Windows Firewall may block other stations: allow [r]evo Slicer on private "
                                                          "networks, or turn sharing on again and accept the administrator prompt");
                        });
                }).detach();
#endif
        };
        if (password.empty())
            return apply("", "");
        std::thread([password, apply] {
            const std::string salt = RevoNet::random_bytes(16), key = RevoNet::derive_key(password, salt);
            wxTheApp->CallAfter([salt, key, apply] { apply(salt, key); });
        }).detach();
    }

    void find()
    {
        std::thread([] {
            const nlohmann::json found = RevoNet::discover(1500);
            wxTheApp->CallAfter([found] {
                nlohmann::json list = nlohmann::json::array();
                for (const auto& s : found)
                    if (s.value("id", "") != g_st.id)
                        list.push_back(s);
                if (s_instance)
                    s_instance->call("MissionApp.onFound", list.dump());
            });
        }).detach();
    }

    void join(const std::string& host, int port, const std::string& password)
    {
        if (g_st.share)
            return result(false, "Turn off sharing first: this computer is a main station");
        std::thread([host, port, password] {
            nlohmann::json info, reply;
            std::string    err, key;
            bool           ok = RevoNet::hello(host, static_cast<unsigned short>(port), info, err);
            if (ok) {
                key = RevoNet::derive_key(password, RevoNet::from_hex(info.value("salt", "")));
                ok  = RevoNet::request(host, static_cast<unsigned short>(port), key, {{"cmd", "ping"}}, reply, err);
            }
            wxTheApp->CallAfter([host, port, info, key, ok, err] {
                if (!s_instance)
                    return;
                if (!ok)
                    return s_instance->result(false, friendly(err));
                if (info.value("id", "") == g_st.id)
                    return s_instance->result(false, "That is this computer");
                g_st.host   = host;
                g_st.port   = port;
                g_st.jkey   = RevoNet::to_hex(key);
                g_st.jname  = info.value("name", host);
                g_st.jid    = info.value("id", "");
                g_connected = true;
                save_station();
                s_instance->result(true, "Connected to " + g_st.jname);
                s_instance->load();
                if (!synced_kinds().empty())
                    s_instance->sync_profiles();
            });
        }).detach();
    }

    void leave()
    {
        g_st.host.clear();
        g_st.jkey.clear();
        g_st.jname.clear();
        g_st.jid.clear();
        g_connected = false;
        save_station();
        g_summary = nullptr;
        send_mission_home(false);
        result(true, "Working with the orders on this computer");
        load();
    }

    // Copies the orders kept on this computer to the main station (both copies are merged).
    void push_local()
    {
        if (!client() || !g_connected)
            return result(false, "Not connected to a main station");
        auto [store, rev] = load_local();
        if (!store.is_object())
            return result(false, "No orders on this computer");
        ask_station({{"cmd", "save"}, {"data", store}}, [](bool ok, const nlohmann::json& reply, const std::string& err) {
            if (!s_instance)
                return;
            if (ok)
                deliver(reply["store"], reply.value("rev", 0LL));
            s_instance->result(ok, ok ? "Orders from this computer merged into the main station" : friendly(err));
        });
    }

    // User presets: the newer file wins on both sides; a file replaced here is first copied to profile-backup.
    void sync_profiles()
    {
        if (!client() || !g_connected)
            return result(false, "Not connected to a main station");
        const std::vector<std::string> kinds = synced_kinds();
        const std::string              host = g_st.host, key = RevoNet::from_hex(g_st.jkey), udir = user_dir(), bdir = backup_dir();
        const unsigned short           port = static_cast<unsigned short>(g_st.port);
        std::thread([kinds, host, port, key, udir, bdir] {
            nlohmann::json remote, reply;
            std::string    err;
            int            pulled = 0, added = 0, pushed = 0, failed = 0;
            bool           ok     = RevoNet::request(host, port, key, {{"cmd", "plist"}}, remote, err, 20);
            if (ok) {
                // macOS and Windows file names ignore case: "PLA.json" here and "pla.json" there are one profile.
                const auto lower = [](std::string p) {
                    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(c < 128 ? std::tolower(c) : c); });
                    return p;
                };
                std::map<std::string, nlohmann::json> here, there;
                for (const auto& f : RevoNet::list_profiles(udir, kinds))
                    here[lower(f["p"].get<std::string>())] = f;
                for (const auto& f : remote.value("list", nlohmann::json::array()))
                    if (std::find(kinds.begin(), kinds.end(), f.value("p", "").substr(0, f.value("p", "").find('/'))) != kinds.end())
                        there[lower(f.value("p", ""))] = f;
                for (const auto& [id, r] : there) {
                    const auto l = here.find(id);
                    if (l != here.end() && (l->second["h"] == r["h"] || l->second["m"].get<int64_t>() > r["m"].get<int64_t>()))
                        continue;
                    const std::string p = l != here.end() ? l->second.value("p", "") : r.value("p", "");   // keep the local name
                    if (RevoNet::request(host, port, key, {{"cmd", "pget"}, {"p", r.value("p", "")}}, reply, err) &&
                        RevoNet::write_profile(udir, bdir, p, reply.value("d", ""), reply.value("m", int64_t(0)))) {
                        ++pulled;
                        added += l == here.end();
                    } else
                        ++failed;
                }
                for (const auto& [id, l] : here) {
                    const auto r = there.find(id);
                    if (r != there.end() && (r->second["h"] == l["h"] || r->second["m"].get<int64_t>() >= l["m"].get<int64_t>()))
                        continue;
                    const std::string p = l.value("p", ""), to = r != there.end() ? r->second.value("p", "") : p;
                    std::string       data;
                    int64_t           mtime = 0;
                    if (RevoNet::read_profile(udir, p, data, mtime) &&
                        RevoNet::request(host, port, key, {{"cmd", "pput"}, {"p", to}, {"d", data}, {"m", mtime}}, reply, err))
                        ++pushed;
                    else
                        ++failed;
                }
            }
            wxTheApp->CallAfter([ok, err, pulled, added, pushed, failed] {
                if (ok && added)
                    load_new_profiles();
                if (ok) {
                    g_st.last_sync = into_u8(wxDateTime::Now().Format("%Y-%m-%d %H:%M"));
                    save_station();
                }
                if (s_instance)
                    s_instance->call("MissionApp.onProfiles",
                                     nlohmann::json({{"ok", ok}, {"error", ok ? "" : friendly(err)}, {"pulled", pulled}, {"added", added},
                                                     {"pushed", pushed}, {"failed", failed}, {"last_sync", g_st.last_sync}})
                                         .dump());
            });
        }).detach();
    }

    void on_message(wxWebViewEvent& evt)
    {
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(into_u8(evt.GetString()));
        } catch (...) {
            return;
        }
        // Replies run after this handler returns: no window work on the stack of a script message.
        CallAfter([this, msg] { handle(msg); });
    }

    void handle(const nlohmann::json& msg)
    {
        const std::string cmd = msg.value("cmd", "");
        if (cmd == "load")
            load();
        else if (cmd == "save" && msg.contains("data"))
            save(msg["data"]);
        else if (cmd == "poll")
            poll();
        else if (cmd == "printers") {
            printers_changed();
            const long long now = wxGetUTCTime();
            if (now - m_refreshed > 15) {
                m_refreshed = now;
                refresh_printers();
            }
        } else if (cmd == "prints")
            fetch_prints(msg.value("since", nlohmann::json::object()));
        else if (cmd == "net")
            net_changed();
        else if (g_demo && (cmd == "net_share" || cmd == "net_join" || cmd == "net_find" || cmd == "net_push_local" || cmd == "prof_sync"))
            result(false, "Not available in presentation mode");
        else if (cmd == "net_share")
            share(msg.value("on", false), msg.value("name", ""), msg.value("password", ""));
        else if (cmd == "net_find")
            find();
        else if (cmd == "net_join")
            join(msg.value("host", ""), msg.value("port", int(RevoNet::HTTP_PORT)), msg.value("password", ""));
        else if (cmd == "net_leave")
            leave();
        else if (cmd == "net_push_local")
            push_local();
        else if (cmd == "prof_set") {
            for (const char* k : {"machine", "process", "filament"})
                g_st.prof[k] = msg.value(k, true);
            save_station();
            net_changed();
        } else if (cmd == "prof_sync")
            sync_profiles();
    }

    // Finished prints from every added printer that answers; offline printers are asked again later.
    void fetch_prints(const nlohmann::json& since)
    {
        if (g_demo) // the sample prints are already in the store; real ones must not land among them
            return;
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
    long long   m_rev{-1};   // revision of the orders the page shows
};

MissionPanel* MissionPanel::s_instance = nullptr;

// A newer copy of the orders reached this computer: the page and the start screen follow.
void deliver(const nlohmann::json& store, long long rev)
{
    g_summary = store.value("podsumowanie", nlohmann::json());
    if (MissionPanel::s_instance)
        MissionPanel::s_instance->merged(store, rev);
    send_mission_home(false);
}

} // namespace

namespace {

// "2026-11-25T14:30:00" for a time `days` from now at `hour`.
std::string demo_at(int days, int hour, int minute = 0)
{
    wxDateTime t = wxDateTime::Today() + wxDateSpan::Days(days);
    t.SetHour(hour).SetMinute(minute);
    return into_u8(t.Format("%Y-%m-%dT%H:%M:%S"));
}
std::string demo_day(int days) { return demo_at(days, 0).substr(0, 10); }

// Sample orders for presentations: every list of the Mission page has something in it, the due dates follow today.
nlohmann::json demo_store()
{
    using json = nlohmann::json;
    const std::vector<std::string> pr = demo_printer_names(); // BELT-01, HALL-A1, HALL-A2, HALL-B1, HALL-A3, BELT-02
    const int year = wxDateTime::Today().GetYear();
    auto numer = [year](int n) { return wxString::Format("ZL/%d/%04d", year, n).ToStdString(); };
    auto item  = [](const char* id, const char* name, int qty, const char* mat, const char* file, const std::string& printer, int good,
                   int bad = 0) {
        return json{{"id", id}, {"nazwa", name}, {"sztuk", qty}, {"material", mat}, {"plik", file}, {"drukarka", printer}, {"dobre", good}, {"wadliwe", bad}};
    };
    auto order = [&](const char* id, int n, const char* client, const char* po, int due, int made, json items, const char* notes = "",
                     bool archived = false) {
        json z = {{"id", id}, {"numer", numer(n)}, {"klient_id", client}, {"po", po}, {"termin", demo_day(due)}, {"notatki", notes},
                  {"pozycje", std::move(items)}, {"utworzone", demo_at(made, 9, 15)}, {"archiwum", archived ? json(demo_at(-2, 16)) : json()},
                  {"historia", json::array({{{"at", demo_at(made, 9, 15)}, {"txt", "Order created"}}})}};
        return z;
    };
    json klienci = json::array({
        {{"id", "k1"}, {"nazwa", "Mechanika Precyzyjna Nowak Sp. z o.o."}, {"email", "zakupy@mp-nowak.example"}, {"tel", "+48 32 555 01 10"}, {"nip", "6340000001"}, {"notatki", "Faktura do 14 dni"}},
        {{"id", "k2"}, {"nazwa", "AutoParts Silesia"}, {"email", "biuro@autoparts.example"}, {"tel", "+48 32 555 02 20"}, {"nip", "6250000002"}, {"notatki", ""}},
        {{"id", "k3"}, {"nazwa", "Studio Projektowe Forma"}, {"email", "studio@forma.example"}, {"tel", "+48 600 555 303"}, {"nip", "9540000003"}, {"notatki", "Prototypy, kolor czarny"}},
        {{"id", "k4"}, {"nazwa", "Laboratorium Robotyki – Politechnika"}, {"email", "lab@uczelnia.example"}, {"tel", "+48 32 555 04 40"}, {"nip", "6310000004"}, {"notatki", ""}},
        {{"id", "k5"}, {"nazwa", "Kraft Gastro GmbH"}, {"email", "einkauf@kraft-gastro.example"}, {"tel", "+49 30 5550 5050"}, {"nip", "DE000000005"}, {"notatki", "Dostawa kurierem"}},
    });
    json zlecenia = json::array({
        // unassigned: items not given to a printer yet
        order("z1", 1, "k3", "SP/118", 9, -1, json::array({item("p11", "Obudowa czujnika – prototyp", 4, "PETG", "obudowa_czujnika.gcode", "", 0),
                                                            item("p12", "Pokrywa z zatrzaskiem", 4, "PETG", "pokrywa.gcode", pr[2], 0)}), "Kolor czarny, matowy"),
        order("z2", 2, "k5", "PO-55871", 16, 0, json::array({item("p21", "Prowadnica kabla 600 mm", 30, "PA-CF", "prowadnica_600.gcode", "", 0)})),
        // assigned: in production
        order("z3", 3, "k1", "ZAM/2026/441", -2, -9, json::array({item("p31", "Uchwyt narzędziowy", 40, "PETG", "uchwyt_narzedziowy.gcode", pr[1], 26, 1),
                                                                   item("p32", "Przyrząd montażowy", 6, "PA-CF", "przyrzad.gcode", pr[3], 2)}), "Pilne – linia stoi"),
        order("z4", 4, "k2", "AP-2026-1093", 3, -5, json::array({item("p41", "Kratka wentylacyjna", 120, "ASA", "kratka.gcode", pr[0], 74, 2)})),
        order("z5", 5, "", "", 12, -3, json::array({item("p51", "Listwa dystansowa – taśma ciągła", 25, "PLA", "listwa_endless.gcode", pr[5], 8),
                                                     item("p52", "Stopka antywibracyjna", 16, "TPU", "stopka.gcode", pr[4], 6)}), "Produkcja własna – magazyn"),
        order("z6", 6, "k4", "", 6, -6, json::array({item("p61", "Ramię chwytaka", 2, "PA-CF", "ramie_chwytaka.gcode", pr[3], 0),
                                                       item("p62", "Osłona przekładni", 4, "PETG", "oslona.gcode", pr[1], 1)})),
        // completed and archived
        order("z7", 7, "k2", "AP-2026-1051", -1, -12, json::array({item("p71", "Zaślepka profilu 40×40", 200, "PETG", "zaslepka_40.gcode", pr[0], 200, 3)})),
        order("z8", 8, "k1", "ZAM/2026/402", -8, -20, json::array({item("p81", "Uchwyt czujnika indukcyjnego", 24, "PETG", "uchwyt_czujnika.gcode", pr[2], 24)}), "", true),
    });
    // finished prints: three wait to be attached, the rest are attached to orders
    auto print = [&](const char* id, const std::string& printer, const std::string& file, int days, int hour, int good, int bad, json to) {
        return json{{"id", std::string("demo#") + id}, {"raport", id}, {"host", "demo"}, {"drukarka", printer}, {"plik", file},
                    {"start", demo_at(days, hour - 2)}, {"koniec", demo_at(days, hour, 40)}, {"stan", "complete"}, {"dobre", good}, {"wadliwe", bad},
                    {"przypisanie", std::move(to)}};
    };
    auto to = [](const char* z, const char* p) { return nlohmann::json{{"typ", "zlecenie"}, {"zlecenie_id", z}, {"pozycja_id", p}}; };
    json wydruki = json::array({
        print("R-1001", pr[1], "probka_klienta.gcode", 0, 11, 1, 0, nullptr),
        print("R-1002", pr[2], "kalibracja_PA-CF.gcode", 0, 8, 1, 0, nullptr),
        print("R-1003", pr[0], "kostki_testowe_x20.gcode", -1, 15, 20, 0, nullptr),
        print("R-0994", pr[0], "ZL-" + std::to_string(year) + "-0004_kratka.gcode", -1, 13, 37, 1, to("z4", "p41")),
        print("R-0990", pr[1], "ZL-" + std::to_string(year) + "-0003_uchwyt_narzedziowy.gcode", -2, 18, 13, 1, to("z3", "p31")),
        print("R-0981", pr[0], "ZL-" + std::to_string(year) + "-0007_zaslepka_40.gcode", -3, 10, 100, 2, to("z7", "p71")),
    });
    json store = {{"v", 1}, {"nr", {{"rok", year}, {"n", 8}}}, {"klienci", klienci}, {"zlecenia", zlecenia}, {"wydruki", wydruki},
                  {"sync", json::object()}, {"usuniete", json::object()}};
    // what the start screen shows until the Mission page saves its own summary
    store["podsumowanie"] = {{"nieprzydzielone", 2}, {"przydzielone", 4}, {"zakonczone", 1}, {"po_terminie", 1}, {"w_tym_tygodniu", 3},
                             {"wydruki_nieprzydzielone", 3}, {"klienci", 5},
                             {"nastepne", {{"numer", numer(3)}, {"nazwa", "Uchwyt narzędziowy"}, {"dobre", 26}, {"sztuk", 40},
                                           {"klient", "Mechanika Precyzyjna Nowak Sp. z o.o."}, {"termin", demo_day(-2)}, {"drukarka", pr[1]}}}};
    return store;
}

} // namespace

bool presentation_mode() { return g_demo; }

void set_presentation_mode(bool on)
{
    if (AppConfig* c = wxGetApp().app_config)
        c->set_bool(DEMO_KEY, on);
    stop_mission_network();
    g_demo = on;
    if (on) {
        // fresh sample data every time, whatever was clicked in the last presentation
        std::lock_guard<std::mutex> lock(g_store_mx);
        write_json(store_file(), demo_store());
        write_json(station_file(), nlohmann::json::object());
        g_rev++;
    }
    g_summary = nullptr;
    start_mission_network();
    if (MissionPanel::s_instance) {
        MissionPanel::s_instance->load();
        MissionPanel::s_instance->net_changed();
        MissionPanel::s_instance->printers_changed();
    }
    send_my_printers(false);
    send_mission_home(false);
}

void start_mission_network()
{
    if (AppConfig* c = wxGetApp().app_config)
        g_demo = c->get_bool(DEMO_KEY);
    if (g_demo && !fs::exists(store_file()))
        write_json(store_file(), demo_store());
    load_station();
    if (!g_demo && g_st.share && !g_st.key.empty() && !client() && !start_server())
        BOOST_LOG_TRIVIAL(error) << "[r]evo Mission: station not started: " << g_server_error;
}

void stop_mission_network()
{
    stop_server();
    g_server.reset();
}

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
    if (g_summary.is_null()) {
        load_station();
        nlohmann::json store = read_json(client() ? cache_file() : store_file());
        if (client() && store.is_object())
            store = store.value("store", nlohmann::json());
        if (store.is_object())
            g_summary = store.value("podsumowanie", nlohmann::json());
    }
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
    // The panel is reused when Mission is switched off and on again (the tab is removed, not deleted).
    // A new main frame (recreate_GUI after a language change) gets its own panel: the old one belongs
    // to the old frame and is destroyed with it.
    if (!MissionPanel::s_instance || MissionPanel::s_instance->GetParent() != parent)
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
