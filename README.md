# Thedas Chronicle

A replacement for the Dragon Age Keep. EA's Keep service no longer works, so Dragon Age: Inquisition can't import a
world state from it. Thedas Chronicle fixes that without the Keep: you record your Origins and Dragon Age II choices on
the Thedas Chronicle website, save them as `world-state.json`, and this plugin hands that file to the game whenever it
asks the Keep for a world state.

This repository holds the game plugin, `ThedasChronicleHook.asi`.

## What the plugin does

- **World state import.** When you start a new game and choose to import from the Keep, the game gets your
  `world-state.json` instead, then shows its usual "custom world state" and "import successful" screens. It works for every
  new game you start, including after quitting to the main menu. Imports only happen in character creation: loading a
  save never changes it.
- **Checks.** During a game started from an import, the plugin checks now and then (a minute in, then every 10 minutes)
  that the imported values are still in place, and writes the result to its log.
- **Golden nug.** By default, using the golden nug unlocks everything it can sync (schematics, mounts, music and the
  rest). Set `Mode=earned` in the settings file to make it work like the original Keep instead: it only unlocks what
  you've synced at the golden nug in your own playthroughs.
- **An offline Keep.** The game never contacts EA's Keep or telemetry servers. The plugin answers every one of those
  requests itself, instantly (login, world state, golden nug, achievements, telemetry), so nothing waits on EA and
  nothing retries. Multiplayer servers are left alone.
- **EA app offline.** With the EA app in offline mode the game normally never signs in to the Keep, so it doesn't ask
  about importing a world state and the golden nug says you're not connected. By default the plugin signs the game in
  to its offline Keep and keeps its Keep requests running, so both work offline too (see Settings).

Everything else in the game is untouched.

## Claim your free Keep rewards now

The Dragon Age Keep website still hands out Inquisition's Keep rewards (items, mounts and the Deluxe and Inquisitor
Edition content) to your EA account. They're EA account entitlements, so the plugin can't provide them; once claimed
they stay on your account and the game picks them up through the EA app, even if the website closes. While
[dragonagekeep.com](https://dragonagekeep.com/en_US/career/#product/dai) is still up, sign in, open **Career** for
Dragon Age: Inquisition and request each unlock.

## Requirements

- Dragon Age: Inquisition for PC: the Steam or EA app version (game executable build `0x5600F278`). Other builds are
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

The game folder is usually:

- Steam: `C:\Program Files (x86)\Steam\steamapps\common\Dragon Age Inquisition`
- EA app: `C:\Program Files\EA Games\Dragon Age Inquisition`

To change your world state, replace `world-state.json` and start a new game. To uninstall, delete
`scripts\ThedasChronicleHook.asi` (and `dinput8.dll` if nothing else uses it).

## Settings

The plugin creates `scripts\ThedasChronicleHook.ini` the first time the game starts. Restart the game after changing it.

```ini
[GoldenNug]
Mode=all

[Offline]
Import=ask
```

- `all` (the default): the golden nug unlocks everything.
- `earned`: like the original Keep. Each time you use the golden nug, that save's unlocks are added to
  `scripts\ThedasChronicleNug.json`, and the golden nug in any playthrough unlocks what's in there. It starts empty, and
  as in the original game the statue only appears after you've finished the main story and used it with a post-game
  character. Unlocks in the file are never removed; delete the file to start over.

`[Offline] Import` decides what happens when the EA app is offline:

- `ask` (the default): the plugin signs the game in to its offline Keep, so the game asks whether to import as usual
  and the golden nug works.
- `always`: `world-state.json` is imported without asking. The golden nug doesn't work while offline.
- `never`: the game's default world state is used. The golden nug doesn't work while offline.

With the EA app online, this setting changes nothing: the game signs in by itself and asks as usual.

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
| `src/hooks_worldstate.cpp` | Signs in to the offline Keep (also when the EA app is offline), imports the file during character creation, turns other world-state replies into a harmless "retry later", and checks the imported values during play. |
| `src/hooks_vault.cpp` | Notes whether the game last asked to read or upload a world state, so replies are handled correctly. |
| `src/hooks_nug.cpp` | Answers the golden nug's sync with "everything unlocked" (`Mode=all`), and lets it run with the EA app offline. |
| `src/hooks_offline.cpp` | The offline Keep: stands in for the game's secure connections to EA's Keep and telemetry servers and returns the plugin's replies (lookups of those servers are also blocked, as a safety net), and keeps the game's Keep requests running when the EA app is offline. |
| `src/keep_answers.cpp` | Writes the offline Keep's reply to each request, and keeps the golden nug's synced unlocks (`Mode=earned`). |
| `src/settings.cpp` | Reads `ThedasChronicleHook.ini`. |
| `src/worldstate_file.cpp` | Reads `world-state.json`. |
| `src/game_hook.cpp` | Finds and hooks game functions safely. |

## Licence

The plugin's own code is released into the public domain under [the Unlicense](LICENSE): use it for anything.
The release build includes MinHook (BSD 2-Clause) and nlohmann/json (MIT); their notices are in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Thedas Chronicle is an unofficial fan project. It is not endorsed by or affiliated with EA or its licensors.
Dragon Age is a trademark of Electronic Arts.
