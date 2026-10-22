#pragma once

namespace hooks::offline {

// Makes the game's Keep (BOS) and telemetry host lookups fail immediately, so those requests fail at once
// instead of waiting on EA's servers; the other hooks then answer them locally. Call early, before the
// game starts networking.
bool Install();

} // namespace hooks::offline
