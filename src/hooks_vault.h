#pragma once

namespace hooks::vault {

// Watches the Keep GameVault requests the game sends, so a failed reply can be matched to its request.
// Call after the game's code has been unpacked.
bool Install();

// True if the most recent GameVault request the game built was an upload (PUT) rather than a read.
bool LastWasUpload();

} // namespace hooks::vault
