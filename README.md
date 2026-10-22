# Thedas Chronicle

A replacement for the Dragon Age Keep. EA's Keep service no longer works, so Dragon Age: Inquisition can't import a
world state from it. Thedas Chronicle fixes that without the Keep: you record your Origins and Dragon Age II choices on
the Thedas Chronicle website, save them as `world-state.json`, and this plugin hands that file to the game whenever it
asks the Keep for a world state.

This repository holds the game plugin, `ThedasChronicleHook.asi`.

## What the plugin does

- **World state import.** When you start a new game and choose to import from the Keep, the game gets your
  `world-state.json` instead, then shows its usual "custom world state" and "import successful" screens. It works for every
  new game you start, including after quitting to the main menu.
- **Golden nug.** Using the golden nug unlocks everything it can sync (schematics, mounts, music and the rest), since there's
  no Keep account to read past unlocks from.
- **No waiting on EA.** The plugin stops the game contacting EA's Keep and telemetry servers, so those requests fail at
  once instead of timing out. Multiplayer servers are left alone.

Everything else in the game is untouched.

## Requirements

- Dragon Age: Inquisition for PC, tested with the Steam version (game executable build `0x5600F278`). Other builds are
  detected and left alone; the log will say so.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (x64).

## Installing

1. Download the x64 **Ultimate ASI Loader** from its [releases page](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases).
   Put its `dinput8.dll` in the game folder, next to `DragonAgeInquisition.exe`.
2. In the game folder, create a folder called `scripts` and put `ThedasChronicleHook.asi` in it. Download it from this
   repository's [releases](../../releases).
3. Make your world state on the Thedas Chronicle website and press **Export JSON**. Put the downloaded
   `world-state.json` in the game folder, next to `DragonAgeInquisition.exe`.
4. Start a new game. At character creation, choose to import a world state from the Keep.

The game folder for the Steam version is usually
`C:\Program Files (x86)\Steam\steamapps\common\Dragon Age Inquisition`.

To change your world state, replace `world-state.json` and start a new game. To uninstall, delete
`scripts\ThedasChronicleHook.asi` (and `dinput8.dll` if nothing else uses it).

## The log

The plugin writes `scripts\ThedasChronicleHook.log` next to itself. Check it if something doesn't work: it says whether
the hooks installed, whether `world-state.json` was found and read, and each import.

## world-state.json

The Thedas Chronicle website writes this file for you. The plugin reads, in this order of preference:

- a `"game"` object: every flag Inquisition reads, including ones derived from your answers (written by the website);
- otherwise an `"answers"` object: the Keep questions only;
- otherwise the whole file as a plain `{"flag_name": value}` object.

Values are integers (booleans work too). `"name"` is shown in game as the world state's name, and `"exported"` (an
ISO 8601 date) as its date.

```json
{
  "name": "My Thedas",
  "exported": "2026-10-22T12:00:00.000Z",
  "game": { "dao_alistair_state": 2, "da2_hawke_mf": 0 }
}
```

## Building from source

You need Visual Studio 2022 or later with the **Desktop development with C++** workload (it includes CMake).
Dependencies ([MinHook](https://github.com/TsudaKageyu/minhook) and [nlohmann/json](https://github.com/nlohmann/json))
are downloaded during the build.

```powershell
.\build.ps1                                  # builds build\Release\ThedasChronicleHook.asi
.\build.ps1 -GameDir "C:\path\to\Dragon Age Inquisition"   # also copies it into <game>\scripts
```

Or with CMake directly: `cmake -S . -B build -A x64` then `cmake --build build --config Release`.

### How it works

The game is packed, so the plugin waits for its code to unpack, checks the first bytes of every function it hooks
against the expected build, and only then hooks them with MinHook.

| File | Purpose |
|---|---|
| `src/hooks_worldstate.cpp` | Fakes a successful Keep login, answers the world-state request with the file, and turns failed uploads into a harmless "retry later". |
| `src/hooks_vault.cpp` | Notes whether the game last asked to read or upload a world state, so replies are handled correctly. |
| `src/hooks_nug.cpp` | Answers the golden nug's sync with "everything unlocked". |
| `src/hooks_offline.cpp` | Makes lookups of EA's Keep and telemetry servers fail immediately. |
| `src/worldstate_file.cpp` | Reads `world-state.json`. |
| `src/game_hook.cpp` | Finds and hooks game functions safely. |

## Licence

The plugin's own code is released into the public domain under [the Unlicense](LICENSE): use it for anything.
The release build includes MinHook (BSD 2-Clause) and nlohmann/json (MIT); their notices are in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Thedas Chronicle is an unofficial fan project. It is not endorsed by or affiliated with EA or its licensors.
Dragon Age is a trademark of Electronic Arts.
