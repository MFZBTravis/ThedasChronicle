#pragma once
#include <windows.h>

namespace game {

// A function inside DragonAgeInquisition.exe, PE timestamp 0x5600F278 (Steam).
// The game is packed, so these only exist in memory after startup; the
// prologue bytes are checked before hooking so a different build is skipped.
struct Target {
    const char* name;
    ULONG_PTR rva;
    BYTE prologue[16];
};

void* Address(const Target& t);

// Waits until the target's code has been unpacked (its prologue matches), up to timeoutMs.
bool WaitForCode(const Target& t, DWORD timeoutMs);

// Verifies the prologue, then creates and enables a MinHook hook.
bool Hook(const Target& t, LPVOID detour, LPVOID* original);

// Installs the in-game hooks on a worker thread once the game has unpacked.
void StartDeferredHooks();

} // namespace game
