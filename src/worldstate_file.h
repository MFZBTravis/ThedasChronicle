#pragma once
#include <optional>
#include <string>
#include <unordered_map>

namespace worldstate {

struct File {
    std::wstring path;
    std::string name;                                     // shown in-game as the world state's name
    std::string updated;                                  // ISO 8601 "YYYY-MM-DDTHH:MM:SS.fff+HH:MM", shown as the import date
    std::unordered_map<std::string, std::string> values;  // game flag name -> decimal value
    bool fromGameSection = false;                         // true if the file had Thedas Chronicle's "game" section
};

// Loads world-state.json from the game's folder (next to DragonAgeInquisition.exe).
// Accepts Thedas Chronicle exports ({"name", "answers", "game"}) or a plain {"flag": value} object.
// Returns nothing (and logs why) if the file is missing or unreadable.
std::optional<File> Load();

// True if world-state.json exists (cheap check, no parsing).
bool Exists();

} // namespace worldstate
