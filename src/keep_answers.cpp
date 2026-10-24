#include "keep_answers.h"

#include "log.h"
#include "settings.h"
#include "worldstate_file.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>

namespace keep {

namespace {

std::atomic<WorldStateRequest> g_request{WorldStateRequest::None};

// The world-state reply as the game's parser reads it: "flags" is an object of flag name -> integer (required),
// "name" and "lastUpdate" are strings, "history" an array of {"name", "hash"}.
std::string WorldStateJson(const std::string& name, const std::string& updated, const nlohmann::json& flags)
{
    nlohmann::json doc = {{"name", name}, {"lastUpdate", updated}, {"flags", flags}, {"history", nlohmann::json::array()}};
    return doc.dump();
}

std::optional<std::string> FileWorldState()
{
    std::optional<worldstate::File> file = worldstate::Load();
    if (!file)
        return std::nullopt;
    nlohmann::json flags = nlohmann::json::object();
    for (const auto& [flag, value] : file->values)
        flags[flag] = strtoll(value.c_str(), nullptr, 10);
    return WorldStateJson(file->name, file->updated, flags);
}

// ---- The golden nug's account file (Mode=earned) ----
// The game reads the file on load and when the nug is used, and uploads it after a sync with the current save's
// unlocks added. The offline Keep keeps it in ThedasChronicleNug.json next to the plugin. With Mode=all the nug hook
// answers reads with everything, so uploads then aren't kept: they'd hold everything, not what was earned.
constexpr const char* kNugFile = "/image/DAINewGamePlusCache";

std::wstring NugPath()
{
    return settings::PluginDir() + L"\\ThedasChronicleNug.json";
}

std::optional<std::string> ReadNug()
{
    std::ifstream in(NugPath(), std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Unlocks are never lost: objects are merged key by key, and a true already kept stays true.
void Merge(nlohmann::json& kept, const nlohmann::json& in)
{
    if (kept.is_object() && in.is_object()) {
        for (const auto& [key, value] : in.items()) {
            if (kept.contains(key))
                Merge(kept[key], value);
            else
                kept[key] = value;
        }
    } else if (!(kept.is_boolean() && kept.get<bool>())) {
        kept = in;
    }
}

void KeepNug(const std::string& body)
{
    std::string out = body;
    nlohmann::json in = nlohmann::json::parse(body, nullptr, false);
    if (std::optional<std::string> old = ReadNug(); old && !in.is_discarded()) {
        nlohmann::json kept = nlohmann::json::parse(*old, nullptr, false);
        if (kept.is_object()) {
            Merge(kept, in);
            out = kept.dump();
        }
    }
    const std::wstring path = NugPath(), temp = path + L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f) {
            LOG("Nug: couldn't write %ls", temp.c_str());
            return;
        }
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    LOG("Nug: synced unlocks kept (%zu bytes uploaded%s)", body.size(), in.is_discarded() ? ", not JSON" : "");
}

} // namespace

Reply Answer(const std::string& method, const std::string& path, const std::string& body)
{
    const std::string route = path.substr(0, path.find('?'));  // query strings may carry ids; keep them out of the log
    Reply r;
    if (route.find(kNugFile) != std::string::npos && settings::NugEarnedOnly()) {
        if (method != "GET") {
            KeepNug(body);
            r.body = "{}";
        } else if (std::optional<std::string> nug = ReadNug()) {
            r.body = std::move(*nug);
        } else {
            r = {404, R"({"error":"not found"})"};  // nothing synced yet
        }
    } else if (route == "/auth") {
        r.body = R"({"sessionId":"thedas-chronicle-offline"})";
    } else if (route.find("/wv/") != std::string::npos && method == "GET") {
        if (std::optional<std::string> ws = FileWorldState()) {
            r.body = std::move(*ws);
            g_request = WorldStateRequest::Served;
        } else {
            r = {404, R"({"error":"not found"})"};
            g_request = WorldStateRequest::NoFile;
        }
    } else if (route.find("/wv/") != std::string::npos) {
        r.body = WorldStateJson("DAI", "", nlohmann::json::object());
    } else {
        r.body = "{}";
    }
    LOG("Keep (offline): %s %s -> %d", method.c_str(), route.c_str(), r.status);
    return r;
}

WorldStateRequest TakeWorldStateRequest()
{
    return g_request.exchange(WorldStateRequest::None);
}

} // namespace keep
