#include "hooks_offline.h"

#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <MinHook.h>

#include <cctype>
#include <cstring>

namespace {

// Hosts the game never reaches: the Dragon Age Keep / BioWare Online Services backend (login, world states,
// golden nug) and EA's telemetry. Multiplayer (Blaze, *.gos.ea.com) is left alone.
const char* const kBlocked[] = {"prd.orbit.bos.ea.com", "emapi.prm.data.ea.com"};

bool IsBlocked(const char* host)
{
    if (!host)
        return false;
    for (const char* b : kBlocked)
        if (_stricmp(host, b) == 0)
            return true;
    return false;
}

using gethostbyname_t = hostent*(WSAAPI*)(const char*);
using getaddrinfo_t = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);

gethostbyname_t o_gethostbyname = nullptr;
getaddrinfo_t o_getaddrinfo = nullptr;

hostent* WSAAPI hk_gethostbyname(const char* name)
{
    if (!IsBlocked(name))
        return o_gethostbyname(name);
    LOG("Offline: blocked lookup of %s", name);
    WSASetLastError(WSAHOST_NOT_FOUND);
    return nullptr;
}

INT WSAAPI hk_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* res)
{
    if (!IsBlocked(node))
        return o_getaddrinfo(node, service, hints, res);
    LOG("Offline: blocked lookup of %s", node);
    if (res)
        *res = nullptr;
    WSASetLastError(WSAHOST_NOT_FOUND);
    return EAI_NONAME;
}

} // namespace

namespace hooks::offline {

bool Install()
{
    LoadLibraryW(L"ws2_32.dll");
    MH_STATUS a = MH_CreateHookApi(L"ws2_32", "gethostbyname", reinterpret_cast<LPVOID>(&hk_gethostbyname),
                                   reinterpret_cast<LPVOID*>(&o_gethostbyname));
    MH_STATUS b = MH_CreateHookApi(L"ws2_32", "getaddrinfo", reinterpret_cast<LPVOID>(&hk_getaddrinfo),
                                   reinterpret_cast<LPVOID*>(&o_getaddrinfo));
    if (a == MH_OK)
        MH_EnableHook(reinterpret_cast<LPVOID>(GetProcAddress(GetModuleHandleW(L"ws2_32"), "gethostbyname")));
    if (b == MH_OK)
        MH_EnableHook(reinterpret_cast<LPVOID>(GetProcAddress(GetModuleHandleW(L"ws2_32"), "getaddrinfo")));
    LOG("Offline hooks (Keep and telemetry servers blocked): gethostbyname %s, getaddrinfo %s", MH_StatusToString(a),
        MH_StatusToString(b));
    return a == MH_OK && b == MH_OK;
}

} // namespace hooks::offline
