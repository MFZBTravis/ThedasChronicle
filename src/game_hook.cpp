#include "game_hook.h"

#include "hooks_worldstate.h"
#include "log.h"

#include <MinHook.h>

#include <cstring>

namespace game {

void* Address(const Target& t)
{
    return reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr)) + t.rva;
}

static bool PrologueMatches(const Target& t)
{
    auto* fn = static_cast<BYTE*>(Address(t));
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(fn, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return memcmp(fn, t.prologue, sizeof(t.prologue)) == 0;
}

bool WaitForCode(const Target& t, DWORD timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    while (!PrologueMatches(t)) {
        if (GetTickCount64() - start > timeoutMs)
            return false;
        Sleep(250);
    }
    LOG("Game code unpacked after %llu ms", GetTickCount64() - start);
    return true;
}

bool Hook(const Target& t, LPVOID detour, LPVOID* original)
{
    if (!PrologueMatches(t)) {
        LOG("  %s: prologue at RVA 0x%llX doesn't match this game build; not hooking", t.name,
            static_cast<unsigned long long>(t.rva));
        return false;
    }
    auto* fn = Address(t);
    MH_STATUS st = MH_CreateHook(fn, detour, original);
    if (st == MH_OK)
        st = MH_EnableHook(fn);
    LOG("  hook %s (%p): %s", t.name, fn, MH_StatusToString(st));
    return st == MH_OK;
}

void StartDeferredHooks()
{
    auto thread = [](LPVOID) -> DWORD {
        hooks::worldstate::Install();
        return 0;
    };
    if (HANDLE h = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr))
        CloseHandle(h);
}

} // namespace game
