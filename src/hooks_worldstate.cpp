#include "hooks_worldstate.h"

#include "game_hook.h"
#include "hooks_nug.h"
#include "hooks_offline.h"
#include "hooks_vault.h"
#include "keep_answers.h"
#include "log.h"
#include "worldstate_file.h"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <cstring>

namespace {

// Layouts recovered from the disassembly (offsets in bytes):
//
// CharGenData reply message (OnMessage's msg)
//   +0x0C  uint32   message type hash
//   +0x28  int32    result: 0 OK, 1 FAILED_RETRY, 2 FAILED_DONOT_RETRY, 3 DISCONNECTED
//   +0x2C  uint8    error flag (Apply requires 0)
//   +0x50  Data*    reply data (Apply requires a valid pointer)
//   +0x60  int32    status (Apply requires 1)
//
// Reply data
//   +0x18  char*    world state name, copied into the manager
//   +0x20  Map      flag name -> value string (ordered map; +0x08 is its end node, +0x18 its root)
//
// Map node, as Apply reads it
//   +0x38  char*    value string, parsed as a number

constexpr uint32_t kMsgCharGenDataResponse = 0xE89C81C4;
constexpr uint32_t kMsgSessionChanged = 0xBD4B8796;    // the game switching state: play starting (new game or save) or quitting to the menu

// The world state is only imported during character creation. Loading a save also asks the Keep for the world state,
// and importing then would overwrite the save's progress. Character creation is recognised by the game resetting the
// world state to defaults from this call site as the screen opens; play starting ends it.
const game::Target kApplyDefaults = {
    "WorldStateManager::ApplyDefaults", 0x33961E4,
    {0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24}};
constexpr ULONG_PTR kCharacterCreationOpensRva = 0x33751BE;  // return address of that ApplyDefaults call

const game::Target kOnMessage = {
    "WorldStateManager::OnMessage", 0x33A2BFB,
    {0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x42, 0x0C, 0x48, 0x89, 0xCB, 0x3D, 0x06, 0xB3, 0x75, 0xDE}};
// BOS (BioWare Online Services, the Keep's backend) response parser:
//   bool Parse(TypeInfo* type, void* out, int httpStatus, const char* error, a5, const char* body, int bodyLength)
// On HTTP 200 it deserialises the JSON body into `out`; otherwise it parses the error and returns false.
// For the login response (`out` = { String sessionId, ... }) a success makes the BOS service record the
// session, switch to "authenticated" and tell the game the player is signed in to the Keep.
const game::Target kBosParse = {
    "BosResponse::Parse", 0x30FF983,
    {0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41}};
constexpr ULONG_PTR kAuthResponseTypeRva = 0x2CFB5B0;  // TypeInfo of the /auth response
const game::Target kStringAssign = {"String::assign", 0x3013681, {}};  // void(String*, const char*, size_t)

const game::Target kMapFind = {
    "StringMap::find", 0x3447AC3,
    {0x55, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x89, 0x5C, 0x24, 0x68}};

struct StrRef {
    const char* begin;
    const char* end;
};

using OnMessage_t = uintptr_t(__fastcall*)(void* self, void* msg);
using ApplyDefaults_t = uintptr_t(__fastcall*)(void* self);
using MapFind_t = void**(__fastcall*)(void* map, void** outNode, const StrRef* key);

using BosParse_t = uintptr_t(__fastcall*)(void* type, void* out, int status, const char* error, uintptr_t a5,
                                          const char* body, uintptr_t bodyLength);
using StringAssign_t = void(__fastcall*)(void* str, const char* text, size_t length);

OnMessage_t o_OnMessage = nullptr;
MapFind_t o_MapFind = nullptr;
BosParse_t o_BosParse = nullptr;

template <typename T>
T& At(void* p, size_t off)
{
    return *reinterpret_cast<T*>(static_cast<BYTE*>(p) + off);
}

// State for one injected import. Only touched on the thread running OnMessage.
struct Injection {
    BYTE data[0x100] = {};  // fake reply data: name at +0x18, empty map at +0x20
    BYTE node[0x80] = {};   // fake map node handed back for each found flag
    worldstate::File file;
    size_t found = 0, missing = 0;
};
Injection* g_active = nullptr;

void* FakeMap(Injection* inj)
{
    return inj->data + 0x20;
}

// Answer Apply's flag lookups from the file; everything else goes to the real function.
void** __fastcall hk_MapFind(void* map, void** outNode, const StrRef* key)
{
    Injection* inj = g_active;
    if (!inj || map != FakeMap(inj))
        return o_MapFind(map, outNode, key);

    std::string name(key->begin, key->end);
    auto it = inj->file.values.find(name);
    if (it == inj->file.values.end()) {
        ++inj->missing;
        *outNode = static_cast<BYTE*>(map) + 0x08;  // end(): the game uses its default for this flag
    } else {
        ++inj->found;
        At<const char*>(inj->node, 0x38) = it->second.c_str();
        *outNode = inj->node;
    }
    return outNode;
}

// Sign in to the Keep: whatever the login (/auth) response held, report success with a placeholder
// session so the game offers its own "import a world state from the Keep?" flow and the golden nug works.
uintptr_t __fastcall hk_BosParse(void* type, void* out, int status, const char* error, uintptr_t a5, const char* body,
                                 uintptr_t bodyLength)
{
    uintptr_t rc = o_BosParse(type, out, status, error, a5, body, bodyLength);
    auto authType = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr)) + kAuthResponseTypeRva;
    if (type != authType)
        return rc;
    static const char kSession[] = "thedas-chronicle-offline";
    reinterpret_cast<StringAssign_t>(game::Address(kStringAssign))(out, kSession, sizeof(kSession) - 1);
    LOG("WorldState: signed in to the offline Keep");
    return 1;
}

ApplyDefaults_t o_ApplyDefaults = nullptr;
std::atomic<bool> g_inCharacterCreation{false};

// ---- Watch: after an import, check now and then that the game still holds the imported values ----
//
// WorldStateManager +0x08 -> config; config +0x20 -> Group*[] (count at [-4]); group +0x28 -> Flag*[];
// flag +0x10 PlotFlagId (type in the low nibble of byte +0x1E: 1 = bool), +0x20 char* name.
// GetPlotSystem() returns the plot system; its vtable +0x00 is GetBool(id), +0x08 is GetInt(id).
// All of this runs on the game thread, from the WorldStateManager's message handler.

const game::Target kGetPlotSystem = {"GetPlotSystem", 0x404EC83, {}};
using GetPlotSystem_t = void*(__fastcall*)();
using GetBool_t = bool(__fastcall*)(void* plot, const void* id);
using GetInt_t = int(__fastcall*)(void* plot, const void* id);

constexpr ULONGLONG kFirstCheckMs = 60 * 1000;        // after play starts (level loading briefly blanks the values)
constexpr ULONGLONG kCheckEveryMs = 10 * 60 * 1000;

// WorldStateManager +0x80: the world state's date, as parsed from the import (year, month, day, time to the
// millisecond, timezone: 0x28 bytes of plain numbers). Each import gets a unique one, so it tells whether the game
// that's loaded is still the one that was imported.
constexpr size_t kDateOffset = 0x80, kDateSize = 0x28;

struct Watch {
    void* manager = nullptr;
    std::unordered_map<std::string, std::string> expected;
    BYTE stamp[kDateSize] = {};
    bool playing = false;    // the imported game has started
    ULONGLONG nextCheck = 0;
};
Watch g_watch;
DWORD g_gameThread = 0;                  // the thread the WorldStateManager handles messages on
// The plot system's GetBool, hooked as a steady tick while an imported game is watched. The hook passes every
// argument register and the whole return value through untouched, and is switched off when watching stops.
using PlotTick_t = uintptr_t(__fastcall*)(void* plot, uintptr_t a2, uintptr_t a3, uintptr_t a4);
PlotTick_t o_PlotTick = nullptr;
void* g_tickFn = nullptr;
bool g_tickOn = false;
thread_local bool t_checking = false;

void CheckImport();

// The game reads plot flags constantly while playing, so GetBool doubles as a clock for the periodic check.
uintptr_t __fastcall hk_PlotTick(void* plot, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    if (!t_checking && g_watch.playing && GetCurrentThreadId() == g_gameThread && GetTickCount64() >= g_watch.nextCheck) {
        g_watch.nextCheck = GetTickCount64() + kCheckEveryMs;
        t_checking = true;
        CheckImport();
        t_checking = false;
    }
    return o_PlotTick(plot, a2, a3, a4);
}

void HookPlotTick()
{
    __try {
        if (!g_tickFn) {
            void* plot = reinterpret_cast<GetPlotSystem_t>(game::Address(kGetPlotSystem))();
            if (!plot)
                return;
            void* fn = (*static_cast<void***>(plot))[0];
            if (MH_CreateHook(fn, reinterpret_cast<LPVOID>(&hk_PlotTick), reinterpret_cast<LPVOID*>(&o_PlotTick)) != MH_OK)
                return;
            g_tickFn = fn;
        }
        MH_STATUS st = MH_EnableHook(g_tickFn);
        g_tickOn = st == MH_OK;
        LOG("WorldState: periodic check hooked to the plot system: %s", MH_StatusToString(st));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void UnhookPlotTick()
{
    if (g_tickOn && MH_DisableHook(g_tickFn) == MH_OK)
        g_tickOn = false;
}

const BYTE* FindFlag(void* manager, const char* name)
{
    void* config = At<void*>(manager, 0x08);
    auto** groups = At<void**>(config, 0x20);
    uint32_t ng = groups ? reinterpret_cast<uint32_t*>(groups)[-1] : 0;
    for (uint32_t g = 0; g < ng; ++g) {
        auto** flags = At<void**>(groups[g], 0x28);
        uint32_t nf = flags ? reinterpret_cast<uint32_t*>(flags)[-1] : 0;
        for (uint32_t i = 0; i < nf; ++i) {
            const char* n = At<const char*>(flags[i], 0x20);
            if (n && strcmp(n, name) == 0)
                return static_cast<const BYTE*>(flags[i]);
        }
    }
    return nullptr;
}

bool StillImportedGame()
{
    return memcmp(static_cast<BYTE*>(g_watch.manager) + kDateOffset, g_watch.stamp, kDateSize) == 0;
}

void StopWatching(const char* why);

void CheckImportUnsafe()
{
    if (!StillImportedGame()) {
        StopWatching("another game is loaded");
        return;
    }
    void* plot = reinterpret_cast<GetPlotSystem_t>(game::Address(kGetPlotSystem))();
    if (!plot)
        return;
    void** vtbl = *static_cast<void***>(plot);
    int ok = 0;
    std::string differ;
    for (const auto& [name, want] : g_watch.expected) {
        const BYTE* flag = FindFlag(g_watch.manager, name.c_str());
        if (!flag)
            continue;  // not one of the flags this game keeps
        const void* id = flag + 0x10;
        int value = (flag[0x1E] & 0xF) == 1 ? (reinterpret_cast<GetBool_t>(vtbl[0])(plot, id) ? 1 : 0)
                                            : reinterpret_cast<GetInt_t>(vtbl[1])(plot, id);
        if (std::to_string(value) == want) {
            ++ok;
        } else {
            differ += " " + name + "=" + std::to_string(value) + " (imported " + want + ")";
        }
    }
    if (differ.empty())
        LOG("WorldState: check: all %d imported flags still as imported", ok);
    else
        LOG("WorldState: check: %d flags as imported, these differ:%s", ok, differ.c_str());
}

void CheckImport()
{
    __try {
        CheckImportUnsafe();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("WorldState: check skipped (the game's world state wasn't readable)");
    }
}

void StopWatching(const char* why)
{
    if (!g_watch.manager)
        return;
    UnhookPlotTick();
    g_watch = {};
    LOG("WorldState: stopped checking the imported world state (%s)", why);
}

uintptr_t __fastcall hk_ApplyDefaults(void* self)
{
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) - base == kCharacterCreationOpensRva) {
        StopWatching("character creation opened");
        if (!g_inCharacterCreation.exchange(true))
            LOG("WorldState: character creation opened; world-state.json will be offered");
    }
    return o_ApplyDefaults(self);
}

uintptr_t __fastcall hk_OnMessage(void* self, void* msg)
{
    g_gameThread = GetCurrentThreadId();
    const uint32_t type = At<uint32_t>(msg, 0x0C);
    if (type == kMsgSessionChanged) {
        if (g_inCharacterCreation.exchange(false))
            LOG("WorldState: character creation finished; imports off until it opens again");
        if (g_watch.manager && g_watch.playing) {
            StopWatching("left the game: quit to the menu or loaded a save");
        } else if (g_watch.manager) {
            g_watch.playing = true;
            g_watch.nextCheck = GetTickCount64() + kFirstCheckMs;
            HookPlotTick();
        }
    }
    if (type != kMsgCharGenDataResponse)
        return o_OnMessage(self, msg);

    const int32_t result = At<int32_t>(msg, 0x28);
    // Results: 0 OK, 1 FAILED_RETRY, 2 FAILED_DONOT_RETRY, 3 DISCONNECTED. The offline Keep answers every request,
    // but the reply is decided here, by the request the game last sent:
    //  - an upload (the game sends the world state back to the Keep after an import and when a save loads):
    //    the game would apply the reply to the loaded game, so report it as "retry later", which changes nothing;
    //  - a read (the player chose to import): answered with world-state.json, but only during character creation.
    //    Anything else (e.g. the request made when a save loads) is also reported as "retry later".
    //  - no request at all: when the player declines the import, the game posts the reply itself without asking
    //    the Keep. That's left alone, so the game uses its default world state.
    if (hooks::vault::LastWasUpload() || !g_inCharacterCreation) {
        At<int32_t>(msg, 0x28) = 1;
        uintptr_t rc = o_OnMessage(self, msg);
        At<int32_t>(msg, 0x28) = result;
        return rc;
    }
    if (!keep::TakeWorldStateServed()) {
        LOG("WorldState: no world state was requested (import declined); the game's default world state is used");
        return o_OnMessage(self, msg);
    }

    std::optional<worldstate::File> file = worldstate::Load();
    if (!file)
        return o_OnMessage(self, msg);

    Injection inj;
    inj.file = std::move(*file);
    At<const char*>(inj.data, 0x18) = inj.file.name.c_str();

    // Make the reply look like a successful Keep answer for the duration of the call.
    // +0x38 is the reply's last-update date, shown in the "import successful" dialogue.
    const int32_t savedResult = result;
    const BYTE savedError = At<BYTE>(msg, 0x2C);
    const int32_t savedStatus = At<int32_t>(msg, 0x60);
    void* const savedData = At<void*>(msg, 0x50);
    const char* const savedDate = At<const char*>(msg, 0x38);
    At<int32_t>(msg, 0x28) = 0;
    At<BYTE>(msg, 0x2C) = 0;
    At<int32_t>(msg, 0x60) = 1;
    At<void*>(msg, 0x50) = inj.data;
    At<const char*>(msg, 0x38) = inj.file.updated.c_str();

    g_active = &inj;
    uintptr_t rc = o_OnMessage(self, msg);
    g_active = nullptr;

    // Put the reply back as it was so its owner frees its own data, not ours.
    At<int32_t>(msg, 0x28) = savedResult;
    At<BYTE>(msg, 0x2C) = savedError;
    At<int32_t>(msg, 0x60) = savedStatus;
    At<void*>(msg, 0x50) = savedData;
    At<const char*>(msg, 0x38) = savedDate;

    LOG("WorldState: imported \"%s\" (dated %s): %zu flags from the file, %zu left at the game's default",
        inj.file.name.c_str(), inj.file.updated.c_str(), inj.found, inj.missing);

    // Re-imports during the same character creation replace the values to watch.
    g_watch = {};
    g_watch.manager = self;
    g_watch.expected = inj.file.values;
    memcpy(g_watch.stamp, static_cast<BYTE*>(self) + kDateOffset, kDateSize);
    return rc;
}

} // namespace

namespace hooks::worldstate {

bool Install()
{
    if (!game::WaitForCode(kOnMessage, 10 * 60 * 1000)) {
        LOG("World state hooks not installed: game code never matched this build");
        return false;
    }
    LOG("Installing world state hooks:");
    bool ok = game::Hook(kMapFind, reinterpret_cast<LPVOID>(&hk_MapFind), reinterpret_cast<LPVOID*>(&o_MapFind));
    ok &= game::Hook(kBosParse, reinterpret_cast<LPVOID>(&hk_BosParse), reinterpret_cast<LPVOID*>(&o_BosParse));
    ok &= game::Hook(kApplyDefaults, reinterpret_cast<LPVOID>(&hk_ApplyDefaults), reinterpret_cast<LPVOID*>(&o_ApplyDefaults));
    // The message hook goes last: it relies on the others being in place.
    ok &= hooks::vault::Install();
    ok = ok && game::Hook(kOnMessage, reinterpret_cast<LPVOID>(&hk_OnMessage), reinterpret_cast<LPVOID*>(&o_OnMessage));
    hooks::nug::Install();
    hooks::offline::InstallGameHooks();
    LOG(ok ? "World state hooks ready; world-state.json is read when a new game starts"
           : "World state hooks incomplete; leaving the game unmodified");
    return ok;
}

} // namespace hooks::worldstate
