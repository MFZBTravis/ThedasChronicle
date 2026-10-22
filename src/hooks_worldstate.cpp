#include "hooks_worldstate.h"

#include "game_hook.h"
#include "hooks_nug.h"
#include "hooks_vault.h"
#include "log.h"
#include "worldstate_file.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

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

// Sign in to the Keep: if the login (/auth) response fails, report success with a placeholder
// session so the game offers its own "import a world state from the Keep?" flow and the golden nug works.
uintptr_t __fastcall hk_BosParse(void* type, void* out, int status, const char* error, uintptr_t a5, const char* body,
                                 uintptr_t bodyLength)
{
    uintptr_t rc = o_BosParse(type, out, status, error, a5, body, bodyLength);
    auto authType = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr)) + kAuthResponseTypeRva;
    if (type != authType || (rc & 0xFF))
        return rc;
    static const char kSession[] = "thedas-chronicle-offline";
    reinterpret_cast<StringAssign_t>(game::Address(kStringAssign))(out, kSession, sizeof(kSession) - 1);
    LOG("WorldState: Keep login faked as successful");
    return 1;
}

uintptr_t __fastcall hk_OnMessage(void* self, void* msg)
{
    if (At<uint32_t>(msg, 0x0C) != kMsgCharGenDataResponse)
        return o_OnMessage(self, msg);

    const int32_t result = At<int32_t>(msg, 0x28);
    // Results: 0 OK, 1 FAILED_RETRY, 2 FAILED_DONOT_RETRY, 3 DISCONNECTED; OK with the error flag set is also
    // a failure. A real answer is left alone. A failure is told apart by the request the game last sent:
    //  - an upload (the game sends the world state back to the Keep after an import and when a save loads):
    //    left alone the game may reset the world state to defaults, so report it as "retry later", which
    //    changes nothing;
    //  - a read (the player chose to import): answered with world-state.json.
    const bool failed = result != 0 || At<BYTE>(msg, 0x2C) != 0;
    if (!failed)
        return o_OnMessage(self, msg);
    if (hooks::vault::LastWasUpload()) {
        At<int32_t>(msg, 0x28) = 1;
        uintptr_t rc = o_OnMessage(self, msg);
        At<int32_t>(msg, 0x28) = result;
        return rc;
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
    // The message hook goes last: it relies on the others being in place.
    ok &= hooks::vault::Install();
    ok = ok && game::Hook(kOnMessage, reinterpret_cast<LPVOID>(&hk_OnMessage), reinterpret_cast<LPVOID*>(&o_OnMessage));
    hooks::nug::Install();
    LOG(ok ? "World state hooks ready; world-state.json is read when a new game starts"
           : "World state hooks incomplete; leaving the game unmodified");
    return ok;
}

} // namespace hooks::worldstate
