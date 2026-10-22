#include "hooks_vault.h"

#include "game_hook.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace {

// BOS GameVault requests: the Keep stores per-account JSON "vaults" (world states among them). The game
// builds each request through the request's vtable:
//   void Build(Request*, int* method, String* url, ?, String* body)   (method 0 GET, 2 PUT)
const game::Target kReadBuild = {
    "BosGameVaultReadRequest::Build", 0x30FFE32,
    {0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x4C, 0x89}};
const game::Target kWriteBuild = {
    "BosGameVaultWriteRequest::Build", 0x30FFEA3,
    {0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70}};

using Build_t = uintptr_t(__fastcall*)(void* req, int* method, void* url, uintptr_t a4, void* body);

Build_t o_ReadBuild = nullptr;
Build_t o_WriteBuild = nullptr;

std::atomic<bool> g_lastWasUpload{false};

uintptr_t __fastcall hk_ReadBuild(void* req, int* method, void* url, uintptr_t a4, void* body)
{
    g_lastWasUpload = false;
    return o_ReadBuild(req, method, url, a4, body);
}

uintptr_t __fastcall hk_WriteBuild(void* req, int* method, void* url, uintptr_t a4, void* body)
{
    g_lastWasUpload = true;
    return o_WriteBuild(req, method, url, a4, body);
}

} // namespace

namespace hooks::vault {

bool LastWasUpload()
{
    return g_lastWasUpload;
}

bool Install()
{
    bool ok = game::Hook(kReadBuild, reinterpret_cast<LPVOID>(&hk_ReadBuild), reinterpret_cast<LPVOID*>(&o_ReadBuild));
    ok &= game::Hook(kWriteBuild, reinterpret_cast<LPVOID>(&hk_WriteBuild), reinterpret_cast<LPVOID*>(&o_WriteBuild));
    return ok;
}

} // namespace hooks::vault
