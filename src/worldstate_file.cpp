#include "worldstate_file.h"

#include "log.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

namespace worldstate {

namespace {

std::wstring GameDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir = buf;
    dir.resize(dir.find_last_of(L"\\/"));
    return dir;
}

// Take integer values from a JSON object into `out`; returns how many were taken.
size_t TakeValues(const nlohmann::json& obj, std::unordered_map<std::string, std::string>& out)
{
    size_t n = 0;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().is_number_integer()) {
            out[it.key()] = std::to_string(it.value().get<long long>());
            ++n;
        } else if (it.value().is_boolean()) {
            out[it.key()] = it.value().get<bool>() ? "1" : "0";
            ++n;
        }
    }
    return n;
}

// The game parses the date as "YYYY-MM-DDTHH:MM:SS.fff+HH:MM". Thedas Chronicle exports
// JavaScript's toISOString() ("...Z"); fall back to the current UTC time if missing or odd.
std::string IsoDate(const std::string& exported)
{
    if (exported.size() >= 23 && exported[4] == '-' && exported[10] == 'T')
        return exported.substr(0, 23) + "+00:00";
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03u+00:00", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds);
    return buf;
}

} // namespace

bool Exists()
{
    return GetFileAttributesW((GameDir() + L"\\world-state.json").c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::optional<File> Load()
{
    File f;
    f.path = GameDir() + L"\\world-state.json";

    std::ifstream in(f.path, std::ios::binary);
    if (!in) {
        LOG("world-state.json not found at %ls; leaving the game's normal behaviour alone", f.path.c_str());
        return std::nullopt;
    }

    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        LOG("world-state.json is not valid JSON; ignoring it");
        return std::nullopt;
    }

    f.name = doc.value("name", std::string("Custom world state"));
    f.updated = IsoDate(doc.value("exported", std::string()));
    if (doc.contains("game") && doc["game"].is_object()) {
        // Thedas Chronicle v2+: every flag Inquisition reads, including derived ones.
        TakeValues(doc["game"], f.values);
        f.fromGameSection = true;
    } else if (doc.contains("answers") && doc["answers"].is_object()) {
        // Older export: Keep answers only. Flags the game derives from them stay at their defaults.
        TakeValues(doc["answers"], f.values);
    } else {
        TakeValues(doc, f.values);
    }

    if (f.values.empty()) {
        LOG("world-state.json has no flag values; ignoring it");
        return std::nullopt;
    }
    LOG("Loaded world-state.json: \"%s\", %zu flags%s", f.name.c_str(), f.values.size(),
        f.fromGameSection ? " (game section)" : " (answers only: derived flags will use defaults)");
    return f;
}

} // namespace worldstate
