#include "settings.h"

#include "log.h"

#include <cstdio>

namespace settings {

namespace {

std::wstring g_dir;
bool g_nugEarned = false;
OfflineImport g_offline = OfflineImport::Ask;

const char kHeader[] = "; Thedas Chronicle plugin settings. Restart the game after changing them.\r\n";

const char kNugSection[] =
    "\r\n"
    "[GoldenNug]\r\n"
    "; all    = the golden nug unlocks everything it can sync (schematics, mounts, music and the rest).\r\n"
    "; earned = like the original Keep: it only unlocks what you've synced at the golden nug in your own\r\n"
    ";          playthroughs. Starts empty: load a save and use the golden nug to add that save's unlocks.\r\n"
    ";          They're kept in ThedasChronicleNug.json, next to this file.\r\n"
    "Mode=all\r\n";

const char kOfflineSection[] =
    "\r\n"
    "[Offline]\r\n"
    "; When the EA app is offline the game doesn't sign in to the Keep, so it can't ask whether to import a\r\n"
    "; world state.\r\n"
    "; ask    = the plugin signs the game in itself, so it asks as normal and the golden nug works.\r\n"
    "; always = import world-state.json without asking.\r\n"
    "; never  = use the game's default world state.\r\n"
    "Import=ask\r\n";

void Append(const std::wstring& path, const char* text, size_t length)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f) {
        fwrite(text, 1, length, f);
        fclose(f);
    }
}

} // namespace

void Load(HMODULE self)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(self, buf, MAX_PATH);
    g_dir = buf;
    g_dir.resize(g_dir.find_last_of(L"\\/"));

    // Write any section the file doesn't have yet (a new file, or one from an older version), with its defaults.
    const std::wstring ini = g_dir + L"\\ThedasChronicleHook.ini";
    const bool exists = GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    wchar_t probe[8] = {};
    if (!exists)
        Append(ini, kHeader, sizeof(kHeader) - 1);
    if (!exists || !GetPrivateProfileSectionW(L"GoldenNug", probe, 8, ini.c_str()))
        Append(ini, kNugSection, sizeof(kNugSection) - 1);
    if (!exists || !GetPrivateProfileSectionW(L"Offline", probe, 8, ini.c_str()))
        Append(ini, kOfflineSection, sizeof(kOfflineSection) - 1);

    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"GoldenNug", L"Mode", L"all", mode, 32, ini.c_str());
    g_nugEarned = _wcsicmp(mode, L"earned") == 0;
    wchar_t offline[32] = {};
    GetPrivateProfileStringW(L"Offline", L"Import", L"ask", offline, 32, ini.c_str());
    g_offline = _wcsicmp(offline, L"always") == 0  ? OfflineImport::Always
                : _wcsicmp(offline, L"never") == 0 ? OfflineImport::Never
                                                   : OfflineImport::Ask;
    LOG("Settings: golden nug unlocks %s; when the EA app is offline, %s",
        g_nugEarned ? "what you've earned (Mode=earned)" : "everything (Mode=all)",
        g_offline == OfflineImport::Always  ? "world-state.json is imported without asking (Import=always)"
        : g_offline == OfflineImport::Never ? "the default world state is used (Import=never)"
                                            : "the plugin signs in to the Keep so the game asks (Import=ask)");
}

const std::wstring& PluginDir()
{
    return g_dir;
}

bool NugEarnedOnly()
{
    return g_nugEarned;
}

OfflineImport Offline()
{
    return g_offline;
}

} // namespace settings
