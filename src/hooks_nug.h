#pragma once

namespace hooks::nug {

// Golden nug: answers the New Game Plus sync's read of the Keep with "everything unlocked" (every plot and
// item the game's nug set contains) and reports its upload as successful. Call after the game has unpacked.
bool Install();

} // namespace hooks::nug
