#pragma once
#include <windows.h>

#include <string>

namespace settings {

// Reads ThedasChronicleHook.ini next to the .asi, writing one with the defaults (and comments) if there isn't one.
void Load(HMODULE self);

// The folder the .asi is in (the game's scripts folder).
const std::wstring& PluginDir();

// [GoldenNug] Mode=earned: the golden nug only unlocks what was synced from your own playthroughs, like the
// original Keep. Mode=all (the default): it unlocks everything.
bool NugEarnedOnly();

} // namespace settings
