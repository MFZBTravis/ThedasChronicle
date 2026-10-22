#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <shlobj.h>
#include <string>

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;

FILE* OpenIn(const std::wstring& dir)
{
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\ThedasChronicleHook.log";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w");
    return f;
}
} // namespace

namespace logging {

void Init(HMODULE self)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(self, buf, MAX_PATH);
    std::wstring dir = buf;
    dir.resize(dir.find_last_of(L"\\/"));

    g_file = OpenIn(dir);
    if (!g_file) {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            g_file = OpenIn(std::wstring(local) + L"\\ThedasChronicleHook");
            CoTaskMemFree(local);
        }
    }
}

void Shutdown()
{
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void Write(const char* fmt, ...)
{
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u][T%05lu] %s\n",
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);

    OutputDebugStringA(line);
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}

void Hex(const void* data, size_t len, size_t maxBytes)
{
    auto* p = static_cast<const unsigned char*>(data);
    size_t n = len < maxBytes ? len : maxBytes;
    for (size_t off = 0; off < n; off += 16) {
        char hex[16 * 3 + 1] = {};
        char asc[17] = {};
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < n) {
                snprintf(hex + i * 3, 4, "%02X ", p[off + i]);
                asc[i] = (p[off + i] >= 0x20 && p[off + i] < 0x7F) ? static_cast<char>(p[off + i]) : '.';
            } else {
                memcpy(hex + i * 3, "   ", 3);
                asc[i] = ' ';
            }
        }
        Write("  %04zX  %s %s", off, hex, asc);
    }
    if (len > n)
        Write("  ... (%zu more bytes)", len - n);
}

} // namespace logging
