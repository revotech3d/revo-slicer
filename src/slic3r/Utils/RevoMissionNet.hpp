#pragma once
// [r]evo Mission on the local network. One slicer is the main station: it keeps the orders and, when the user turns
// sharing on, serves them to the other slicers in the building. The others find it with a UDP broadcast and log in
// with the station password. The password never travels: both sides derive an AES-256-GCM key from it (PBKDF2 with
// the station's salt) and every request and reply is sealed with that key, so orders, client data and printer keys in
// synced profiles are never readable on the network, and a request that does not open is refused.
//
// This file has no wx dependency so the protocol can be tested on its own.

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r::RevoNet {

constexpr unsigned short HTTP_PORT      = 13660;
constexpr unsigned short DISCOVERY_PORT = 13661;

// ---------- crypto ----------
std::string random_bytes(size_t n);
std::string to_hex(const std::string& bytes);
std::string from_hex(const std::string& hex);
std::string sha256_hex(const std::string& data);
// 32-byte key from the station password and salt.
std::string derive_key(const std::string& password, const std::string& salt);
// {"iv":…,"ct":…} (base64); open() fails when the key is wrong or the data was changed.
std::string seal(const std::string& key, const std::string& plaintext);
bool        open(const std::string& key, const std::string& envelope, std::string& plaintext);

// ---------- orders ----------
// Merges two copies of the Mission store. Clients, orders and prints are matched by id and the newer one (field "zm",
// ms) wins; a deletion is a tombstone in "usuniete" {id: ms} that removes copies not changed after it. Two stations
// that numbered an order with the same number while apart: the later order gets the next free number.
nlohmann::json merge_store(const nlohmann::json& current, const nlohmann::json& incoming);

// ---------- user profiles ----------
// Relative paths of user presets that may be synced: "<machine|process|filament>/[base/]<name>.json". Only the .json
// files travel; the .info files hold cloud ids of this computer and are never copied.
bool           valid_profile_path(const std::string& rel);
// [{"p": rel, "m": mtime (s), "h": sha256}] for the given kinds under the user preset folder.
nlohmann::json list_profiles(const std::string& user_dir, const std::vector<std::string>& kinds);
bool           read_profile(const std::string& user_dir, const std::string& rel, std::string& data, int64_t& mtime);
// Writes the profile with the given mtime; an existing file is first copied to backup_dir/<rel>.
bool           write_profile(const std::string& user_dir, const std::string& backup_dir, const std::string& rel,
                             const std::string& data, int64_t mtime);

// ---------- main station ----------
class Server
{
public:
    // Runs on the server thread with the opened request; returns the reply to seal.
    using Handler = std::function<nlohmann::json(const nlohmann::json& request)>;

    Server();
    ~Server();
    // Listens on all interfaces (HTTP_PORT) and answers discovery (DISCOVERY_PORT). False with error set when the port
    // is taken or the network is not available.
    bool start(const std::string& key, const std::string& salt, const std::string& name, const std::string& id,
               Handler handler, std::string& error);
    void stop();
    bool running() const { return m_running; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::thread           m_thread;
    std::atomic<bool>     m_running{false};
};

// ---------- other stations ----------
// Stations that answered a broadcast within timeout_ms: [{"name","host","port","id"}].
nlohmann::json discover(int timeout_ms);
// Unsealed station info: {"app","name","id","salt"}.
bool hello(const std::string& host, unsigned short port, nlohmann::json& info, std::string& error);
// Sealed request to a station; the reply must answer this request.
bool request(const std::string& host, unsigned short port, const std::string& key, nlohmann::json req,
             nlohmann::json& reply, std::string& error, int timeout_s = 10);
// Addresses of this computer on the local network, for the user to type on another station.
std::vector<std::string> local_addresses();

} // namespace Slic3r::RevoNet
