#pragma once

namespace hooks::worldstate {

// Makes Dragon Age: Inquisition import world-state.json as if the Dragon Age Keep had answered.
//
// When the WorldStateManager receives the CharGenData reply (which now fails, because EA's
// server errors), the reply is turned into a success carrying the file's world-state name,
// and the game's own import code (Apply) runs. Apply looks each flag up by name in the reply's
// flag map; during that call the lookups are answered from the file. Flags the file doesn't
// have fall back to the game's defaults, exactly like a real Keep import.
//
// Waits for the game's code to be unpacked, then installs. Returns false if this game build
// doesn't match the expected code.
bool Install();

// True once the game is signed in to the (offline) Keep.
bool SignedIn();

} // namespace hooks::worldstate
