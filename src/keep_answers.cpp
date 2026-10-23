#include "keep_answers.h"

#include "log.h"
#include "worldstate_file.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <optional>

namespace keep {

namespace {

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

} // namespace

Reply Answer(const std::string& method, const std::string& path, const std::string& /*body*/)
{
    const std::string route = path.substr(0, path.find('?'));  // query strings may carry ids; keep them out of the log
    Reply r;
    if (route == "/auth") {
        r.body = R"({"sessionId":"thedas-chronicle-offline"})";
    } else if (route.find("/wv/") != std::string::npos && method == "GET") {
        if (std::optional<std::string> ws = FileWorldState())
            r.body = std::move(*ws);
        else
            r = {404, R"({"error":"not found"})"};
    } else if (route.find("/wv/") != std::string::npos) {
        r.body = WorldStateJson("DAI", "", nlohmann::json::object());
    } else {
        r.body = "{}";
    }
    LOG("Keep (offline): %s %s -> %d", method.c_str(), route.c_str(), r.status);
    return r;
}

} // namespace keep
