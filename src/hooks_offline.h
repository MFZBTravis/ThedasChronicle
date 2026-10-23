#pragma once

namespace hooks::offline {

// Makes the game's Keep (BOS) and telemetry host lookups fail immediately, as a safety net in case a request
// gets past the offline Keep below. Call early, before the game starts networking.
bool Install();

// The offline Keep (game-code hooks, after unpacking): requests to the blocked servers are answered by the plugin
// at the TLS-socket level (see keep_answers.h), at once and without network traffic.
bool InstallGameHooks();

} // namespace hooks::offline
