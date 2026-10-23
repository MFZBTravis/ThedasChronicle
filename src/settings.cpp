#include "settings.h"

#include "log.h"

#include <cstdio>

namespace settings {

namespace {

std::wstring g_dir;
bool g_nugEarned = false;

const char kDefaultIni[] =
    "; Thedas Chronicle plugin settings. Restart the game after changing them.\r\n"
    "\r\n"
    "[GoldenNug]\r\n"
    "; all    = the golden nug unlocks everything it can sync (schematics, mounts, music and the rest).\r\n"
    "; earned = like the original Keep: it only unlocks what you've synced at the golden nug in your own\r\n"
    ";          playthroughs. Starts empty: load a save and use the golden nug to add that save's unlocks.\r\n"
    ";          They're kept in ThedasChronicleNug.json, next to this file.\r\n"
    "Mode=all\r\n";

} // namespace

void Load(HMODULE self)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(self, buf, MAX_PATH);
    g_dir = buf;
    g_dir.resize(g_dir.find_last_of(L"\\/"));

    const std::wstring ini = g_dir + L"\\ThedasChronicleHook.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, ini.c_str(), L"wb") == 0 && f) {
            fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, f);
            fclose(f);
        }
    }
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"GoldenNug", L"Mode", L"all", mode, 32, ini.c_str());
    g_nugEarned = _wcsicmp(mode, L"earned") == 0;
    LOG("Settings: golden nug unlocks %s", g_nugEarned ? "what you've earned (Mode=earned)" : "everything (Mode=all)");
}

const std::wstring& PluginDir()
{
    return g_dir;
}

bool NugEarnedOnly()
{
    return g_nugEarned;
}

} // namespace settings
