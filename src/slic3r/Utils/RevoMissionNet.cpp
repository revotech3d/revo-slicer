#include "RevoMissionNet.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Slic3r::RevoNet {

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace fs    = boost::filesystem;
using net::ip::tcp;
using net::ip::udp;
using nlohmann::json;

namespace {

constexpr int         PBKDF2_ROUNDS   = 200000;
constexpr size_t      BODY_LIMIT      = 64 * 1024 * 1024;
constexpr int64_t     MAX_CLOCK_SKEW  = 5 * 60 * 1000;               // a sealed request older than this is refused
constexpr int64_t     TOMBSTONE_KEEP  = 180LL * 24 * 3600 * 1000;     // deletions remembered for half a year
constexpr const char* DISCOVER_ASK    = "REVO-MISSION?";
const char* const     COLLECTIONS[]   = {"klienci", "zlecenia", "wydruki"};
const char* const     PROFILE_KINDS[] = {"machine", "process", "filament"};

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string base64(const std::string& in)
{
    std::string out(4 * ((in.size() + 2) / 3) + 1, '\0');
    const int   n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), reinterpret_cast<const unsigned char*>(in.data()),
                                    int(in.size()));
    out.resize(n);
    return out;
}

bool unbase64(const std::string& in, std::string& out)
{
    if (in.size() % 4)
        return false;
    out.assign(in.size() / 4 * 3 + 1, '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&out[0]), reinterpret_cast<const unsigned char*>(in.data()), int(in.size()));
    if (n < 0)
        return false;
    size_t pad = 0;
    for (auto it = in.rbegin(); it != in.rend() && *it == '=' && pad < 2; ++it)
        ++pad;
    out.resize(size_t(n) - pad);
    return true;
}

int64_t zm(const json& e)
{
    const auto it = e.find("zm");
    return it != e.end() && it->is_number() ? it->get<int64_t>() : 0;
}

std::string iso_now()
{
    const std::time_t t = std::time(nullptr);
    std::tm           tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

// "ZL/2026/0042" -> {2026, 42}
bool parse_number(const std::string& s, int& year, int& n)
{
    return s.size() == 12 && s.compare(0, 3, "ZL/") == 0 && s[7] == '/' && std::sscanf(s.c_str() + 3, "%4d/%4d", &year, &n) == 2;
}

std::string format_number(int year, int n)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "ZL/%04d/%04d", year, n);
    return buf;
}

// Orders that ended up with one number on two stations: the one created first keeps it.
void renumber(json& store)
{
    json& orders = store["zlecenia"];
    json& nr     = store["nr"];
    if (!orders.is_array())
        return;
    if (!nr.is_object())
        nr = json{{"rok", 0}, {"n", 0}};
    const int        rok = nr.value("rok", 0);
    int              top = nr.value("n", 0);
    std::set<std::string> used;
    std::map<std::string, std::vector<size_t>> by_number;
    for (size_t i = 0; i < orders.size(); ++i) {
        const std::string num = orders[i].value("numer", "");
        int               y, n;
        if (parse_number(num, y, n) && y == rok)
            top = std::max(top, n);
        used.insert(num);
        by_number[num].push_back(i);
    }
    for (auto& [num, idx] : by_number) {
        if (idx.size() < 2 || num.empty())
            continue;
        std::sort(idx.begin(), idx.end(), [&orders](size_t a, size_t b) {
            const auto ka = std::make_pair(orders[a].value("utworzone", ""), orders[a].value("id", ""));
            const auto kb = std::make_pair(orders[b].value("utworzone", ""), orders[b].value("id", ""));
            return ka < kb;
        });
        for (size_t k = 1; k < idx.size(); ++k) {
            std::string fresh;
            do
                fresh = format_number(rok, ++top);
            while (used.count(fresh));
            used.insert(fresh);
            json& z = orders[idx[k]];
            if (!z["historia"].is_array())
                z["historia"] = json::array();
            z["historia"].insert(z["historia"].begin(),
                                 json{{"at", iso_now()}, {"txt", "Number " + num + " was taken on another station, renumbered " + fresh}});
            z["numer"] = fresh;
            z["zm"]    = now_ms();
        }
    }
    nr["n"] = top;
}

} // namespace

// ---------- crypto ----------

std::string random_bytes(size_t n)
{
    std::string s(n, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(&s[0]), int(n));
    return s;
}

std::string to_hex(const std::string& bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string        out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string from_hex(const std::string& hex)
{
    std::string out;
    if (hex.size() % 2)
        return out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned v = 0;
        if (std::sscanf(hex.c_str() + i, "%2x", &v) != 1)
            return {};
        out += char(v);
    }
    return out;
}

std::string sha256_hex(const std::string& data)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr);
    return to_hex(std::string(reinterpret_cast<char*>(md), len));
}

std::string derive_key(const std::string& password, const std::string& salt)
{
    std::string key(32, '\0');
    PKCS5_PBKDF2_HMAC(password.data(), int(password.size()), reinterpret_cast<const unsigned char*>(salt.data()), int(salt.size()),
                      PBKDF2_ROUNDS, EVP_sha256(), 32, reinterpret_cast<unsigned char*>(&key[0]));
    return key;
}

std::string seal(const std::string& key, const std::string& plaintext)
{
    const std::string iv = random_bytes(12);
    std::string       ct(plaintext.size() + 16, '\0');
    int               len = 0, total = 0;
    EVP_CIPHER_CTX*   ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                       reinterpret_cast<const unsigned char*>(iv.data()));
    EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(&ct[0]), &len, reinterpret_cast<const unsigned char*>(plaintext.data()),
                      int(plaintext.size()));
    total = len;
    EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&ct[0]) + total, &len);
    total += len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, &ct[total]);
    EVP_CIPHER_CTX_free(ctx);
    ct.resize(size_t(total) + 16);
    return json{{"iv", base64(iv)}, {"ct", base64(ct)}}.dump();
}

bool open(const std::string& key, const std::string& envelope, std::string& plaintext)
{
    std::string iv, ct;
    try {
        const json e = json::parse(envelope);
        if (!unbase64(e.at("iv").get<std::string>(), iv) || !unbase64(e.at("ct").get<std::string>(), ct))
            return false;
    } catch (...) {
        return false;
    }
    if (iv.size() != 12 || ct.size() < 16 || key.size() != 32)
        return false;
    const size_t    n = ct.size() - 16;
    std::string     out(n + 16, '\0');
    int             len = 0, total = 0;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                       reinterpret_cast<const unsigned char*>(iv.data()));
    EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&out[0]), &len, reinterpret_cast<const unsigned char*>(ct.data()), int(n));
    total = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, &ct[n]);
    const bool ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]) + total, &len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    out.resize(size_t(total + len));
    plaintext = std::move(out);
    return true;
}

// ---------- orders ----------

json merge_store(const json& current, const json& incoming)
{
    const auto valid = [](const json& s) { return s.is_object() && s.contains("v"); };
    if (!valid(incoming))
        return current;
    if (!valid(current)) {
        json out = incoming;
        renumber(out);
        return out;
    }
    json out = current;

    json          tomb   = json::object();
    const int64_t oldest = now_ms() - TOMBSTONE_KEEP;
    for (const json* s : {&current, &incoming})
        if (const auto it = s->find("usuniete"); it != s->end() && it->is_object())
            for (const auto& [id, t] : it->items())
                if (t.is_number() && t.get<int64_t>() > oldest && (!tomb.contains(id) || tomb[id].get<int64_t>() < t.get<int64_t>()))
                    tomb[id] = t;

    for (const char* k : COLLECTIONS) {
        std::vector<json>             list;
        std::map<std::string, size_t> index;
        for (const json* s : {&current, &incoming}) {
            const auto it = s->find(k);
            if (it == s->end() || !it->is_array())
                continue;
            for (const json& e : *it) {
                const std::string id = e.is_object() ? e.value("id", "") : "";
                if (id.empty())
                    continue;
                const auto found = index.find(id);
                if (found == index.end()) {
                    index[id] = list.size();
                    list.push_back(e);
                } else if (zm(e) >= zm(list[found->second]))
                    list[found->second] = e;
            }
        }
        json arr = json::array();
        for (json& e : list) {
            const std::string id = e.value("id", "");
            if (!(tomb.contains(id) && tomb[id].get<int64_t>() >= zm(e)))
                arr.push_back(std::move(e));
        }
        out[k] = std::move(arr);
    }
    out["usuniete"] = tomb;

    const auto nr_key = [](const json& s) {
        const json nr = s.value("nr", json::object());
        return std::make_pair(nr.is_object() ? nr.value("rok", 0) : 0, nr.is_object() ? nr.value("n", 0) : 0);
    };
    if (nr_key(incoming) > nr_key(current))
        out["nr"] = incoming["nr"];

    json sync = current.value("sync", json::object());
    if (!sync.is_object())
        sync = json::object();
    if (const auto it = incoming.find("sync"); it != incoming.end() && it->is_object())
        for (const auto& [host, last] : it->items())
            if (!sync.contains(host) || (last.is_string() && sync[host].is_string() && last.get<std::string>() > sync[host].get<std::string>()))
                sync[host] = last;
    out["sync"] = sync;

    if (incoming.contains("podsumowanie"))
        out["podsumowanie"] = incoming["podsumowanie"];
    out["v"] = std::max(current.value("v", 1), incoming.value("v", 1));
    renumber(out);
    return out;
}

// ---------- user profiles ----------

bool valid_profile_path(const std::string& rel)
{
    std::vector<std::string> parts;
    std::stringstream        ss(rel);
    for (std::string p; std::getline(ss, p, '/');)
        parts.push_back(p);
    if (parts.size() < 2 || parts.size() > 3 || (parts.size() == 3 && parts[1] != "base"))
        return false;
    if (std::find(std::begin(PROFILE_KINDS), std::end(PROFILE_KINDS), parts[0]) == std::end(PROFILE_KINDS))
        return false;
    const std::string& name = parts.back();
    return name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0 && name[0] != '.' && rel.find("..") == std::string::npos &&
           rel.find('\\') == std::string::npos && rel.find(':') == std::string::npos;
}

json list_profiles(const std::string& user_dir, const std::vector<std::string>& kinds)
{
    json out = json::array();
    for (const std::string& kind : kinds)
        for (const std::string sub : {"", "/base"}) {
            const fs::path dir = fs::path(user_dir) / (kind + sub);
            boost::system::error_code ec;
            if (!fs::is_directory(dir, ec))
                continue;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                const std::string name = e.path().filename().string();
                const std::string rel  = kind + sub + "/" + name;
                if (!fs::is_regular_file(e.path(), ec) || !valid_profile_path(rel))
                    continue;
                std::string data;
                int64_t     mtime = 0;
                if (read_profile(user_dir, rel, data, mtime))
                    out.push_back({{"p", rel}, {"m", mtime}, {"h", sha256_hex(data)}});
            }
        }
    return out;
}

bool read_profile(const std::string& user_dir, const std::string& rel, std::string& data, int64_t& mtime)
{
    if (!valid_profile_path(rel))
        return false;
    const fs::path            file = fs::path(user_dir) / rel;
    boost::nowide::ifstream   f(file.string(), std::ios::binary);
    if (!f)
        return false;
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    boost::system::error_code ec;
    mtime = int64_t(fs::last_write_time(file, ec));
    return !ec;
}

bool write_profile(const std::string& user_dir, const std::string& backup_dir, const std::string& rel, const std::string& data,
                   int64_t mtime)
{
    if (!valid_profile_path(rel))
        return false;
    try {
        if (!json::accept(data))   // never write something the slicer could not load
            return false;
        const fs::path file = fs::path(user_dir) / rel;
        fs::create_directories(file.parent_path());
        if (fs::exists(file)) {
            const fs::path backup = fs::path(backup_dir) / rel;
            fs::create_directories(backup.parent_path());
            fs::copy_file(file, backup, fs::copy_options::overwrite_existing);
        }
        const fs::path tmp = file.string() + ".tmp";
        {
            boost::nowide::ofstream f(tmp.string(), std::ios::binary | std::ios::trunc);
            f << data;
            if (!f)
                return false;
        }
        fs::rename(tmp, file);
        fs::last_write_time(file, std::time_t(mtime));
        return true;
    } catch (...) {
        return false;
    }
}

// ---------- main station ----------

struct Server::Impl
{
    net::io_context io;
    tcp::acceptor   acceptor{io};
    udp::socket     udp_socket{io};
    udp::endpoint   udp_from;
    std::array<char, 64> udp_buf{};
    std::string     key, salt, name, id;
    Handler         handler;

    struct Session : std::enable_shared_from_this<Session>
    {
        Session(tcp::socket s, Impl* impl) : stream(std::move(s)), impl(impl) {}
        beast::tcp_stream                                       stream;
        beast::flat_buffer                                      buffer;
        std::optional<http::request_parser<http::string_body>> parser;
        http::response<http::string_body>                       res;
        Impl*                                                   impl;

        void run()
        {
            parser.emplace();
            parser->body_limit(BODY_LIMIT);
            stream.expires_after(std::chrono::seconds(30));
            http::async_read(stream, buffer, *parser, [self = shared_from_this()](beast::error_code ec, size_t) {
                if (ec)
                    return;
                self->res = self->impl->answer(self->parser->get());
                self->res.prepare_payload();
                http::async_write(self->stream, self->res, [self](beast::error_code, size_t) {
                    beast::error_code ignored;
                    self->stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
                });
            });
        }
    };

    http::response<http::string_body> answer(const http::request<http::string_body>& req)
    {
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "application/json");
        const std::string target(req.target());
        if (req.method() == http::verb::get && target == "/mission/hello") {
            res.body() = json{{"app", "revo-mission"}, {"v", 1}, {"name", name}, {"id", id}, {"salt", to_hex(salt)}}.dump();
            return res;
        }
        if (req.method() != http::verb::post || target != "/mission") {
            res.result(http::status::not_found);
            return res;
        }
        std::string plain;
        json        in;
        try {
            if (!open(key, req.body(), plain))
                throw std::runtime_error("auth");
            in = json::parse(plain);
            if (std::llabs(in.value("ts", int64_t(0)) - now_ms()) > MAX_CLOCK_SKEW)
                throw std::runtime_error("clock");
        } catch (const std::exception& e) {
            res.result(http::status::unauthorized);
            res.body() = json{{"error", e.what()}}.dump();
            return res;
        }
        json out;
        try {
            out = handler(in);
        } catch (const std::exception& e) {
            out = json{{"error", e.what()}};
        }
        out["re"] = in.value("n", "");
        res.body() = seal(key, out.dump());
        return res;
    }

    void accept()
    {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket s) {
            if (ec == net::error::operation_aborted)
                return;
            if (!ec)
                std::make_shared<Session>(std::move(s), this)->run();
            accept();
        });
    }

    void listen_udp()
    {
        udp_socket.async_receive_from(net::buffer(udp_buf), udp_from, [this](beast::error_code ec, size_t n) {
            if (ec == net::error::operation_aborted)
                return;
            if (!ec && std::string(udp_buf.data(), n) == DISCOVER_ASK) {
                const std::string reply = json{{"app", "revo-mission"}, {"name", name}, {"id", id}, {"port", HTTP_PORT}}.dump();
                beast::error_code ignored;
                udp_socket.send_to(net::buffer(reply), udp_from, 0, ignored);
            }
            listen_udp();
        });
    }
};

Server::Server() = default;
Server::~Server() { stop(); }

bool Server::start(const std::string& key, const std::string& salt, const std::string& name, const std::string& id, Handler handler,
                   std::string& error)
{
    stop();
    m_impl          = std::make_unique<Impl>();
    m_impl->key     = key;
    m_impl->salt    = salt;
    m_impl->name    = name;
    m_impl->id      = id;
    m_impl->handler = std::move(handler);
    try {
        const tcp::endpoint ep(tcp::v4(), HTTP_PORT);
        m_impl->acceptor.open(ep.protocol());
        m_impl->acceptor.set_option(net::socket_base::reuse_address(true));
        m_impl->acceptor.bind(ep);
        m_impl->acceptor.listen();
        m_impl->udp_socket.open(udp::v4());
        m_impl->udp_socket.set_option(net::socket_base::reuse_address(true));
        m_impl->udp_socket.bind(udp::endpoint(udp::v4(), DISCOVERY_PORT));
    } catch (const std::exception& e) {
        error    = e.what();
        m_impl.reset();
        return false;
    }
    m_impl->accept();
    m_impl->listen_udp();
    m_running = true;
    m_thread  = std::thread([impl = m_impl.get()] { impl->io.run(); });
    return true;
}

void Server::stop()
{
    if (!m_impl)
        return;
    net::post(m_impl->io, [impl = m_impl.get()] {
        beast::error_code ignored;
        impl->acceptor.close(ignored);
        impl->udp_socket.close(ignored);
        impl->io.stop();
    });
    if (m_thread.joinable())
        m_thread.join();
    m_impl.reset();
    m_running = false;
}

// ---------- other stations ----------

json discover(int timeout_ms)
{
    json found = json::array();
    try {
        net::io_context io;
        udp::socket     sock(io, udp::endpoint(udp::v4(), 0));
        sock.set_option(net::socket_base::broadcast(true));
        sock.send_to(net::buffer(std::string(DISCOVER_ASK)), udp::endpoint(net::ip::address_v4::broadcast(), DISCOVERY_PORT));
        std::array<char, 1024> buf{};
        udp::endpoint          from;
        std::set<std::string>  seen;
        std::function<void()>  receive = [&] {
            sock.async_receive_from(net::buffer(buf), from, [&](beast::error_code ec, size_t n) {
                if (ec)
                    return;
                try {
                    json j = json::parse(std::string(buf.data(), n));
                    if (j.value("app", "") == "revo-mission" && seen.insert(j.value("id", "")).second)
                        found.push_back({{"name", j.value("name", "")}, {"id", j.value("id", "")},
                                         {"host", from.address().to_string()}, {"port", j.value("port", int(HTTP_PORT))}});
                } catch (...) {}
                receive();
            });
        };
        receive();
        io.run_for(std::chrono::milliseconds(timeout_ms));
    } catch (...) {}
    return found;
}

namespace {

bool http_call(const std::string& host, unsigned short port, http::verb verb, const std::string& target, const std::string& body,
               int timeout_s, unsigned& status, std::string& reply, std::string& error)
{
    try {
        net::io_context   io;
        tcp::resolver     resolver(io);
        beast::tcp_stream stream(io);
        stream.expires_after(std::chrono::seconds(timeout_s));
        stream.connect(resolver.resolve(host, std::to_string(port)));
        http::request<http::string_body> req{verb, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
        http::write(stream, req);
        beast::flat_buffer                        buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(BODY_LIMIT);
        http::read(stream, buffer, parser);
        status = parser.get().result_int();
        reply  = parser.get().body();
        beast::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace

bool hello(const std::string& host, unsigned short port, json& info, std::string& error)
{
    unsigned    status = 0;
    std::string body;
    if (!http_call(host, port, http::verb::get, "/mission/hello", "", 4, status, body, error))
        return false;
    try {
        info = json::parse(body);
        if (status == 200 && info.value("app", "") == "revo-mission")
            return true;
    } catch (...) {}
    error = "not a [r]evo Mission station";
    return false;
}

bool request(const std::string& host, unsigned short port, const std::string& key, json req, json& reply, std::string& error, int timeout_s)
{
    const std::string nonce = to_hex(random_bytes(8));
    req["n"]                = nonce;
    req["ts"]               = now_ms();
    unsigned    status      = 0;
    std::string body;
    if (!http_call(host, port, http::verb::post, "/mission", seal(key, req.dump()), timeout_s, status, body, error))
        return false;
    if (status == 401) {
        try {
            error = json::parse(body).value("error", "auth");
        } catch (...) {
            error = "auth";
        }
        return false;
    }
    std::string plain;
    if (status != 200 || !open(key, body, plain)) {
        error = "auth";
        return false;
    }
    try {
        reply = json::parse(plain);
    } catch (...) {
        error = "bad reply";
        return false;
    }
    if (reply.value("re", "") != nonce) {
        error = "bad reply";
        return false;
    }
    if (reply.contains("error")) {
        error = reply["error"].get<std::string>();
        return false;
    }
    return true;
}

std::vector<std::string> local_addresses()
{
    std::vector<std::string> out;
    try {
        // No packet is sent: connecting a UDP socket only picks the interface that routes outwards.
        net::io_context io;
        udp::socket     s(io);
        s.connect(udp::endpoint(net::ip::make_address("192.0.2.1"), 9));
        out.push_back(s.local_endpoint().address().to_string());
    } catch (...) {}
    return out;
}

} // namespace Slic3r::RevoNet
