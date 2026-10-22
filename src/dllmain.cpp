#include "game_hook.h"
#include "hooks_offline.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>

namespace {

HMODULE g_self = nullptr;

DWORD WINAPI InitThread(LPVOID)
{
    logging::Init(g_self);
    LOG("ThedasChronicleHook " THEDAS_VERSION " loaded (built " __DATE__ " " __TIME__ ")");

    HMODULE exe = GetModuleHandleW(nullptr);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(exe) + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    LOG("Game PE timestamp: 0x%08X (hooks are written for 0x5600F278)", nt->FileHeader.TimeDateStamp);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK) {
        LOG("MH_Initialize failed: %s", MH_StatusToString(st));
        return 1;
    }
    hooks::offline::Install();
    game::StartDeferredHooks();
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = module;
        DisableThreadLibraryCalls(module);
        // Don't install hooks under the loader lock; do it on a worker thread.
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr))
            CloseHandle(t);
        break;
    case DLL_PROCESS_DETACH:
        // On process exit (reserved != null) other threads are already gone;
        // just let the OS clean up rather than unhooking.
        if (!reserved) {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
        }
        logging::Shutdown();
        break;
    }
    return TRUE;
}
