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

// [Offline] Import: what happens when the game isn't signed in to the Keep (the EA app is offline), so it can't ask
// whether to import a world state.
//   Ask     the plugin signs the game in to the offline Keep itself, so it asks as normal (and the golden nug works)
//   Always  world-state.json is imported without asking
//   Never   the game's default world state is used
enum class OfflineImport { Ask, Always, Never };
OfflineImport Offline();

// [Promo] Unlock=yes: the Keep's rewards that can't be claimed any more (The Last Court's Serault missions, the
// promotional rings, amulet and mounts) are always available. Off by default.
bool PromoUnlock();

} // namespace settings
