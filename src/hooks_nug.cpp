#include "hooks_nug.h"

#include "game_hook.h"
#include "log.h"
#include "settings.h"

#include <windows.h>

#include <cstdint>

namespace {

// The golden nug (New Game Plus sync) keeps the account's unlocks in a JSON file on the Keep:
//   { "Plots": { "<plot name>": true, ... }, "Items": { "<item id>": true, ... } }
// Its manager reads that file, grants every plot and item it knows about whose entry is true, then
// exports the current save's unlocks and uploads them.
//
// Keep file reply message (type hash 0x4F8AF7FA), as the manager reads it:
//   +0x30  int32    0 = reply to a read, 1 = reply to a write
//   +0x34  uint8    error flag
//   +0x38  int32    HTTP status (404 on a read means "nothing synced yet")
//   +0x48  char*    file path
//   +0x50  char*    body
//   +0x58  int32    body length
constexpr uint32_t kMsgKeepFileReply = 0x4F8AF7FA;

const game::Target kNugOnMessage = {
    "NewGamePlusManager::OnMessage", 0x33B8FCF,
    {0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD0}};
// JsonNode* GetBool(JsonDocument*, const char* pointer): the node at the JSON pointer if it is a bool, else null.
// The import reads the value byte at +0x40.
const game::Target kJsonGetBool = {
    "Json::GetBool", 0x374CF36,
    {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x87, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x06, 0x83, 0x78}};

using OnMessage_t = uintptr_t(__fastcall*)(void* self, void* msg);
using JsonGetBool_t = void*(__fastcall*)(void* doc, const char* pointer);

OnMessage_t o_NugOnMessage = nullptr;
JsonGetBool_t o_JsonGetBool = nullptr;

template <typename T>
T& At(void* p, size_t off)
{
    return *reinterpret_cast<T*>(static_cast<BYTE*>(p) + off);
}

// Set while the manager handles a read reply: every "/Plots/..." and "/Items/..." lookup answers true.
thread_local bool t_grantAll = false;
thread_local unsigned t_plots = 0, t_items = 0;

alignas(16) BYTE g_trueNode[0x80] = {};  // JSON bool node: type 4 at +0x08, value at +0x40

void* __fastcall hk_JsonGetBool(void* doc, const char* pointer)
{
    if (!t_grantAll || !pointer)
        return o_JsonGetBool(doc, pointer);
    if (strncmp(pointer, "/Plots/", 7) == 0)
        ++t_plots;
    else if (strncmp(pointer, "/Items/", 7) == 0)
        ++t_items;
    else
        return o_JsonGetBool(doc, pointer);
    return g_trueNode;
}

uintptr_t __fastcall hk_NugOnMessage(void* self, void* msg)
{
    if (At<uint32_t>(msg, 0x0C) != kMsgKeepFileReply)
        return o_NugOnMessage(self, msg);

    const int32_t kind = At<int32_t>(msg, 0x30);
    const BYTE error = At<BYTE>(msg, 0x34);
    if (kind != 0 && kind != 1)
        return o_NugOnMessage(self, msg);
    // Mode=earned: the offline Keep answers with the unlocks saved from earlier syncs, and the game handles that as is.
    if (settings::NugEarnedOnly())
        return o_NugOnMessage(self, msg);

    // Present the reply as a success. Reads get an empty JSON object as their body; the lookups then
    // answer true for everything, so the game grants every plot and item the golden nug can sync.
    static const char kEmpty[] = "{}";
    const char* savedBody = At<const char*>(msg, 0x50);
    const int32_t savedLength = At<int32_t>(msg, 0x58);
    At<BYTE>(msg, 0x34) = 0;
    if (kind == 0) {
        At<const char*>(msg, 0x50) = kEmpty;
        At<int32_t>(msg, 0x58) = sizeof(kEmpty) - 1;
        t_grantAll = true;
        t_plots = t_items = 0;
    }

    uintptr_t rc = o_NugOnMessage(self, msg);

    t_grantAll = false;
    At<BYTE>(msg, 0x34) = error;
    At<const char*>(msg, 0x50) = savedBody;
    At<int32_t>(msg, 0x58) = savedLength;
    if (kind == 0)
        LOG("Nug: answered the read with everything unlocked (%u plots, %u items offered)", t_plots, t_items);
    return rc;
}

} // namespace

namespace hooks::nug {

bool Install()
{
    At<int32_t>(g_trueNode, 0x08) = 4;
    g_trueNode[0x40] = 1;
    bool ok = game::Hook(kJsonGetBool, reinterpret_cast<LPVOID>(&hk_JsonGetBool), reinterpret_cast<LPVOID*>(&o_JsonGetBool));
    ok = ok && game::Hook(kNugOnMessage, reinterpret_cast<LPVOID>(&hk_NugOnMessage), reinterpret_cast<LPVOID*>(&o_NugOnMessage));
    return ok;
}

} // namespace hooks::nug
