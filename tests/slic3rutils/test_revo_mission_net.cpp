#include <catch2/catch_test_macros.hpp>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#include "slic3r/Utils/RevoMissionNet.hpp"

using namespace Slic3r::RevoNet;
using nlohmann::json;

namespace {

int64_t recent_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 100000;
}

json order(const std::string& id, const std::string& number, const std::string& created, int64_t zm)
{
    return {{"id", id}, {"numer", number}, {"utworzone", created}, {"zm", zm}, {"pozycje", json::array()}};
}

// A braced single order would become that object itself, not a one-element array, on some compilers:
// callers pass json::array({...}).
json store(json orders)
{
    return {{"v", 1}, {"nr", {{"rok", 2026}, {"n", 2}}}, {"klienci", json::array()}, {"wydruki", json::array()}, {"zlecenia", orders}};
}

} // namespace

TEST_CASE("A sealed message opens only with the key it was sealed with", "[RevoMissionNet]")
{
    const std::string salt = random_bytes(16);
    const std::string key  = derive_key("station password", salt);
    const std::string env  = seal(key, "orders");
    std::string       out;
    CHECK(open(key, env, out));
    CHECK(out == "orders");
    CHECK_FALSE(open(derive_key("other password", salt), env, out));
    CHECK(derive_key("station password", salt) == key);
}

TEST_CASE("The newer copy of an order wins the merge", "[RevoMissionNet]")
{
    const int64_t t   = recent_ms();
    json          a   = store(json::array({order("z1", "ZL/2026/0001", "2026-11-01T10:00:00", t)}));
    json          b   = a;
    b["zlecenia"][0]["po"] = "PO-7";
    b["zlecenia"][0]["zm"] = t + 10;
    CHECK(merge_store(a, b)["zlecenia"][0]["po"] == "PO-7");
    CHECK(merge_store(b, a)["zlecenia"][0]["po"] == "PO-7");
}

TEST_CASE("A deletion removes copies not changed after it", "[RevoMissionNet]")
{
    const int64_t t = recent_ms();
    json          a = store(json::array({order("z1", "ZL/2026/0001", "2026-11-01T10:00:00", t)}));
    json          b = store(json::array());
    b["usuniete"]   = {{"z1", t + 5}};
    CHECK(merge_store(a, b)["zlecenia"].empty());
    a["zlecenia"][0]["zm"] = t + 9;   // edited after the deletion: kept
    CHECK(merge_store(a, b)["zlecenia"].size() == 1);
}

TEST_CASE("Orders numbered alike on two stations get distinct numbers", "[RevoMissionNet]")
{
    const int64_t t = recent_ms();
    const json    a = store(json::array({order("z1", "ZL/2026/0002", "2026-11-01T10:00:00", t)}));
    const json    b = store(json::array({order("z2", "ZL/2026/0002", "2026-11-01T11:00:00", t)}));
    const json    m = merge_store(a, b);
    REQUIRE(m["zlecenia"].size() == 2);
    CHECK(m["zlecenia"][0]["numer"] == "ZL/2026/0002");   // created first, keeps its number
    CHECK(m["zlecenia"][1]["numer"] == "ZL/2026/0003");
    CHECK(m["nr"]["n"] == 3);
}

TEST_CASE("Only user preset json files can be synced", "[RevoMissionNet]")
{
    CHECK(valid_profile_path("machine/My printer.json"));
    CHECK(valid_profile_path("filament/base/PLA.json"));
    CHECK_FALSE(valid_profile_path("machine/../secret.json"));
    CHECK_FALSE(valid_profile_path("process/Fine.info"));
    CHECK_FALSE(valid_profile_path("other/x.json"));
}

TEST_CASE("A profile name has to be a valid file name on both macOS and Windows", "[RevoMissionNet]")
{
    CHECK(valid_profile_path("process/0.20mm Standard @Revon.json"));
    CHECK_FALSE(valid_profile_path("process/a:b.json"));
    CHECK_FALSE(valid_profile_path("process/what?.json"));
    CHECK_FALSE(valid_profile_path("process/quoted \"x\".json"));
    CHECK_FALSE(valid_profile_path("process/ends with space .json"));
}

TEST_CASE("A synced profile keeps its time and backs up the one it replaces", "[RevoMissionNet]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
    const std::string             user = (root / "user").string(), backup = (root / "backup").string();
    REQUIRE(write_profile(user, backup, "process/Fine.json", R"({"a":1})", 1700000000));
    CHECK_FALSE(write_profile(user, backup, "process/Broken.json", "not json", 1700000000));
    REQUIRE(write_profile(user, backup, "process/Fine.json", R"({"a":2})", 1700000100));
    const json list = list_profiles(user, {"process"});
    REQUIRE(list.size() == 1);
    CHECK(list[0]["m"] == 1700000100);
    CHECK(boost::filesystem::exists(root / "backup" / "process" / "Fine.json"));
    boost::filesystem::remove_all(root);
}

TEST_CASE("A station answers only requests sealed with its password", "[RevoMissionNet]")
{
    const std::string salt = random_bytes(16), key = derive_key("station password", salt);
    Server            server;
    std::string       error;
    REQUIRE(server.start(key, salt, "Office", "id-1", [](const json& r) { return json{{"echo", r.value("x", 0)}}; }, error));
    json info;
    REQUIRE(hello("127.0.0.1", HTTP_PORT, info, error));
    CHECK(info["name"] == "Office");
    json reply;
    CHECK(request("127.0.0.1", HTTP_PORT, derive_key("station password", from_hex(info["salt"])), {{"x", 7}}, reply, error));
    CHECK(reply["echo"] == 7);
    CHECK_FALSE(request("127.0.0.1", HTTP_PORT, derive_key("wrong", salt), {{"x", 7}}, reply, error));
    CHECK(error == "auth");
    server.stop();
}
