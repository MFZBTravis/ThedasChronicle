#include "hooks_offline.h"

#include "game_hook.h"
#include "keep_answers.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <MinHook.h>

#include <algorithm>
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

} // namespace

namespace hooks::offline {

bool InstallGameHooks()
{
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
