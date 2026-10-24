#include "hooks_offline.h"

#include "game_hook.h"
#include "keep_answers.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

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

// ---- The offline Keep ----
//
// The game's Keep and telemetry requests go through DirtySDK's ProtoHttp, which talks to the server through
// ProtoSSL (TLS sockets). For the blocked hosts the plugin stands in for ProtoSSL: Connect opens nothing, Send
// collects the request, and Recv hands back a complete HTTP reply from keep::Answer. ProtoHttp parses that reply as
// if a server had sent it, so the game's own code (jobs, callbacks, parsers) runs as it would with a live Keep,
// and every request completes at once. The DNS block above stays as a safety net.
//   int  ProtoSSLConnect(ssl, int secure, const char* host, uint32 addr, int port)
//   int  ProtoSSLStat(ssl, int select, void* buf, int size)   'stat' 1 = connected, 'fail' 0 = no error
//   int  ProtoSSLSend(ssl, const char* data, int len)          bytes accepted (len < 0: NUL-terminated)
//   int  ProtoSSLRecv(ssl, char* buf, int len)                 bytes, 0 = nothing yet, -1 = closed
//   int  ProtoSSLDisconnect(ssl)
//   void ProtoSSLDestroy(ssl)
const game::Target kSslConnect = {"ProtoSSLConnect", 0x3718D36,
    {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x44, 0x89, 0xCE, 0x4C}};
const game::Target kSslStat = {"ProtoSSLStat", 0x37196F5,
    {0x53, 0x48, 0x83, 0xEC, 0x40, 0x45, 0x89, 0xCA, 0x4D, 0x89, 0xC1, 0x41, 0x83, 0xC8, 0xFF, 0x48}};
const game::Target kSslSend = {"ProtoSSLSend", 0x37194D7,
    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}};
const game::Target kSslRecv = {"ProtoSSLRecv", 0x37193C3,
    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}};
const game::Target kSslDisconnect = {"ProtoSSLDisconnect", 0x3719332,
    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x31, 0xFF, 0x48, 0x89, 0xCB, 0x48}};
const game::Target kSslDestroy = {"ProtoSSLDestroy", 0x3719303,
    {0x53, 0x48, 0x83, 0xEC, 0x20, 0x31, 0xD2, 0x48, 0x89, 0xCB, 0xE8, 0xFB, 0x65, 0x00, 0x00, 0x4C}};

using SslConnect_t = int(__fastcall*)(void* ssl, int secure, const char* host, uint32_t addr, int port);
using SslStat_t = int(__fastcall*)(void* ssl, int sel, void* buf, int size);
using SslSend_t = int(__fastcall*)(void* ssl, const char* data, int len);
using SslRecv_t = int(__fastcall*)(void* ssl, char* buf, int len);
using SslDisconnect_t = int(__fastcall*)(void* ssl);
using SslDestroy_t = void(__fastcall*)(void* ssl);
SslConnect_t o_SslConnect = nullptr;
SslStat_t o_SslStat = nullptr;
SslSend_t o_SslSend = nullptr;
SslRecv_t o_SslRecv = nullptr;
SslDisconnect_t o_SslDisconnect = nullptr;
SslDestroy_t o_SslDestroy = nullptr;

constexpr int FourCC(const char (&c)[5]) { return (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]; }

struct FakeConn {
    std::string out;    // request bytes received so far
    std::string reply;  // the whole HTTP reply, once the request is complete
    size_t sent = 0;    // reply bytes handed to the game
    bool ready = false;
};
std::mutex g_fakeMutex;
std::unordered_map<void*, FakeConn> g_fake;  // ProtoSSL ref -> fake connection

bool IsFakeHost(const char* host)
{
    if (!host)
        return false;
    std::string h(host, strcspn(host, ":"));
    return IsBlocked(h.c_str());
}

std::string Header(const std::string& head, const char* name)
{
    const size_t n = strlen(name);
    for (size_t pos = head.find("\r\n"); pos != std::string::npos; pos = head.find("\r\n", pos + 2)) {
        if (_strnicmp(head.c_str() + pos + 2, name, n) == 0 && head.compare(pos + 2 + n, 1, ":") == 0) {
            size_t v = head.find_first_not_of(' ', pos + 3 + n);
            if (v == std::string::npos)
                return {};
            return head.substr(v, head.find("\r\n", v) - v);
        }
    }
    return {};
}

// Once the whole request (header and body) has arrived, work out the reply.
void TryComplete(FakeConn& c)
{
    const size_t end = c.out.find("\r\n\r\n");
    if (end == std::string::npos)
        return;
    const std::string head = c.out.substr(0, end);
    const size_t length = strtoull(Header(head, "Content-Length").c_str(), nullptr, 10);
    if (c.out.size() < end + 4 + length)
        return;
    const std::string line = head.substr(0, head.find("\r\n"));
    const size_t sp1 = line.find(' '), sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 <= sp1)
        return;
    keep::Reply r = keep::Answer(line.substr(0, sp1), line.substr(sp1 + 1, sp2 - sp1 - 1), c.out.substr(end + 4, length));
    const char* reason = r.status == 200 ? "OK" : r.status == 404 ? "Not Found" : "Error";
    c.reply = "HTTP/1.1 " + std::to_string(r.status) + " " + reason +
              "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(r.body.size()) +
              "\r\nConnection: close\r\n\r\n" + r.body;
    c.sent = 0;
    c.ready = true;
    c.out.clear();
}

int __fastcall hk_SslConnect(void* ssl, int secure, const char* host, uint32_t addr, int port)
{
    {
        std::lock_guard lock(g_fakeMutex);
        if (IsFakeHost(host)) {
            g_fake[ssl] = FakeConn{};
            return 0;  // nothing opened: no lookup, no socket
        }
        g_fake.erase(ssl);
    }
    return o_SslConnect(ssl, secure, host, addr, port);
}

int __fastcall hk_SslStat(void* ssl, int sel, void* buf, int size)
{
    {
        std::lock_guard lock(g_fakeMutex);
        if (g_fake.count(ssl)) {
            if (buf && size > 0)
                memset(buf, 0, size);
            return sel == FourCC("stat") ? 1 : 0;
        }
    }
    return o_SslStat(ssl, sel, buf, size);
}

int __fastcall hk_SslSend(void* ssl, const char* data, int len)
{
    {
        std::lock_guard lock(g_fakeMutex);
        auto it = g_fake.find(ssl);
        if (it != g_fake.end()) {
            if (len < 0)
                len = static_cast<int>(strlen(data));
            it->second.out.append(data, len);
            TryComplete(it->second);
            return len;
        }
    }
    return o_SslSend(ssl, data, len);
}

int __fastcall hk_SslRecv(void* ssl, char* buf, int len)
{
    {
        std::lock_guard lock(g_fakeMutex);
        auto it = g_fake.find(ssl);
        if (it != g_fake.end()) {
            FakeConn& c = it->second;
            if (!c.ready)
                return 0;
            const int n = static_cast<int>(std::min<size_t>(len, c.reply.size() - c.sent));
            if (n <= 0)
                return -1;  // everything delivered: the "server" closed the connection
            memcpy(buf, c.reply.data() + c.sent, n);
            c.sent += n;
            if (n < len)
                buf[n] = 0;
            return n;
        }
    }
    return o_SslRecv(ssl, buf, len);
}

int __fastcall hk_SslDisconnect(void* ssl)
{
    {
        std::lock_guard lock(g_fakeMutex);
        if (g_fake.erase(ssl))
            return 0;
    }
    return o_SslDisconnect(ssl);
}

void __fastcall hk_SslDestroy(void* ssl)
{
    {
        std::lock_guard lock(g_fakeMutex);
        g_fake.erase(ssl);
    }
    o_SslDestroy(ssl);
}

// ---- Keeping the Keep's HTTP client running when the EA app is offline ----
//
// Every Keep request ends up as a job on an HTTP client (vtable 0x1FFF3B0: +0x20 Get, +0x28 Post, +0x30 Post with
// a body object). The client runs its jobs in Update(client, float dt), but when the EA app is offline nothing calls
// Update for it, so the jobs never start. The online manager's own Update(manager, float dt) still runs every frame
// on the same thread; if the client that last took a job hasn't been updated for a second, the plugin updates it
// from there.
const game::Target kClientGet = {"HttpClient::Get", 0x3C58C4F,
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE}};
const game::Target kClientPost = {"HttpClient::Post", 0x3C5A992,
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE}};
const game::Target kClientPost2 = {"HttpClient::Post2", 0x3C5A880,
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE}};
const game::Target kClientUpdate = {"HttpClient::Update", 0x3C5C964,
    {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x59, 0x08, 0x48, 0x8D}};
const game::Target kManagerUpdate = {"OnlineManager::Update", 0x310D5B7,
    {0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0xCB, 0x48, 0x81, 0xC1, 0x78, 0x02, 0x00, 0x00, 0x0F}};
constexpr ULONGLONG kClientIdleMs = 1000;

using AddJob_t = uintptr_t(__fastcall*)(void* client, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);
using Update_t = void(__fastcall*)(void* self, float dt);
AddJob_t o_ClientGet = nullptr;
AddJob_t o_ClientPost = nullptr;
AddJob_t o_ClientPost2 = nullptr;
Update_t o_ClientUpdate = nullptr;
Update_t o_ManagerUpdate = nullptr;
std::atomic<void*> g_keepClient{nullptr};
std::atomic<ULONGLONG> g_keepClientUpdated{0};
bool g_drivingNoted = false;

// A client the plugin hasn't seen yet gets a second's grace: the game may simply not have updated it yet.
void NoteClient(void* client)
{
    if (g_keepClient.exchange(client) != client)
        g_keepClientUpdated = GetTickCount64();
}

uintptr_t __fastcall hk_ClientGet(void* client, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    NoteClient(client);
    return o_ClientGet(client, a2, a3, a4, a5);
}

uintptr_t __fastcall hk_ClientPost(void* client, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    NoteClient(client);
    return o_ClientPost(client, a2, a3, a4, a5);
}

uintptr_t __fastcall hk_ClientPost2(void* client, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    NoteClient(client);
    return o_ClientPost2(client, a2, a3, a4, a5);
}

void __fastcall hk_ClientUpdate(void* client, float dt)
{
    if (client == g_keepClient)
        g_keepClientUpdated = GetTickCount64();
    o_ClientUpdate(client, dt);
}

void __fastcall hk_ManagerUpdate(void* manager, float dt)
{
    o_ManagerUpdate(manager, dt);
    void* client = g_keepClient;
    if (!client || GetTickCount64() - g_keepClientUpdated < kClientIdleMs)
        return;
    if (!g_drivingNoted) {
        g_drivingNoted = true;
        LOG("Offline Keep: the game isn't running its Keep requests (EA app offline?); running them from the plugin");
    }
    o_ClientUpdate(client, dt);  // the original, so this doesn't count as the game updating it
}

} // namespace

namespace hooks::offline {

bool InstallGameHooks()
{
    game::Hook(kClientGet, reinterpret_cast<LPVOID>(&hk_ClientGet), reinterpret_cast<LPVOID*>(&o_ClientGet));
    game::Hook(kClientPost, reinterpret_cast<LPVOID>(&hk_ClientPost), reinterpret_cast<LPVOID*>(&o_ClientPost));
    game::Hook(kClientPost2, reinterpret_cast<LPVOID>(&hk_ClientPost2), reinterpret_cast<LPVOID*>(&o_ClientPost2));
    game::Hook(kClientUpdate, reinterpret_cast<LPVOID>(&hk_ClientUpdate), reinterpret_cast<LPVOID*>(&o_ClientUpdate));
    game::Hook(kManagerUpdate, reinterpret_cast<LPVOID>(&hk_ManagerUpdate), reinterpret_cast<LPVOID*>(&o_ManagerUpdate));
    bool ok = game::Hook(kSslConnect, reinterpret_cast<LPVOID>(&hk_SslConnect), reinterpret_cast<LPVOID*>(&o_SslConnect));
    ok &= game::Hook(kSslStat, reinterpret_cast<LPVOID>(&hk_SslStat), reinterpret_cast<LPVOID*>(&o_SslStat));
    ok &= game::Hook(kSslSend, reinterpret_cast<LPVOID>(&hk_SslSend), reinterpret_cast<LPVOID*>(&o_SslSend));
    ok &= game::Hook(kSslRecv, reinterpret_cast<LPVOID>(&hk_SslRecv), reinterpret_cast<LPVOID*>(&o_SslRecv));
    ok &= game::Hook(kSslDisconnect, reinterpret_cast<LPVOID>(&hk_SslDisconnect), reinterpret_cast<LPVOID*>(&o_SslDisconnect));
    ok &= game::Hook(kSslDestroy, reinterpret_cast<LPVOID>(&hk_SslDestroy), reinterpret_cast<LPVOID*>(&o_SslDestroy));
    return ok;
}

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
