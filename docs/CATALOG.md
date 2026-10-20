# Port Manager: catalog format and notes

Technical notes for `catalog.json` and how ports fit into Onion. For players: `../README.md`.
Plan: `PLAN.md`; decisions, findings and the state of the owner's card: `NOTES.md`.

## Rules

- **Everything goes to Onion's Ports list**: game in `Roms/PORTS/Games/<dir>/`, shortcut in
  `Roms/PORTS/Shortcuts/<Category>/<Name>.port`, picture in `Roms/PORTS/Imgs/<Name>.png`.
  Nothing in `App/`, even for ports whose own zips install there (miyoo64).
- **Copyrighted files (ROMs, game data) are never downloaded or hosted**: the player copies
  them; every entry says where to get them legally and where they go (`get`).
- Files the player added, changed or created (data, saves, settings) are never overwritten or
  deleted; replaced files are backed up first.
- **Only clean ports** (owner, 2026-10-19):
  - open source, with the source on GitHub (not only release zips);
  - native, with no Java emulation (FreeJ2ME) or other emulation or compatibility layer;
  - preparing the player's own game files on a PC is fine (Zuma Deluxe's `Launcher.py`
    packs them and converts the music): Port Manager never installs game data anyway, and
    `get` describes the step. The port's program itself must come from a release Port
    Manager can install.
  Apps are out anyway (first rule).

## catalog.json

`catalog.schema.json` is the format as a JSON Schema (draft 7): allowed fields, categories,
layouts and statuses, and paths that stay inside the game folder. CI (`catalog.yml`) checks
`catalog.json` against it, plus unique ids and asset regexes that compile. Every device
fetches `catalog.json` from `main`, so a wrong field would reach every player.

`catalog.local.json` (git-ignored): entries for this card only, e.g. ports from private repos.
`src/build.sh` copies it from the repo root to `App/PortManager/` next to `catalog.json`.
Port Manager reads it after the catalog every time, and a local entry with the same `id`
replaces the catalog's. The catalog fetched from GitHub never touches it, and neither does
self-update. Same format and schema.

`sources`: other catalogs entries were checked against, by hand; Port Manager doesn't read it.
[MiyooMini-Ports][mmp] (MIT, 66 entries,
2026-10-17) already lists Miyoo Mini ports by their authors' `/releases` pages, with `name`,
`categories`, `status` (playable/experimental/prerelease/source-only), `assets` (free/owned),
`porter`, `image`, `notes`. It has no install data, so our entries add it, matched by repo.
Of its 66 entries ~55 have a release; asset formats vary (zip, 7z, tar, tar.gz, ipk) and many
releases carry builds for other systems (spruce, Allium, PortMaster), so each entry names its
asset by a regex.

`ports[]`:

| field | meaning |
|---|---|
| `id` | unique, lowercase |
| `name` | display name = shortcut and picture file name |
| `category` | Shortcuts subfolder (see below) |
| `status` | optional: `playable`, `experimental` (runs, but rough) or `prerelease` (the author calls it a test version); the last two are shown on the game's page, all three in its details |
| `repo` | `owner/name` on GitHub; the latest release is used; the owner is shown as the port's author |
| `asset` | regex for the release file; `null` = no releases yet |
| `layout` | `sdcard-root`: the zip mirrors the card (`Roms/PORTS/...`); its shortcuts go to `category` (a `.notfound` one as `.port`, unless the zip has the `.port` too); `app-to-ports`: the zip has `App/<dir>/` (miyoo64), which goes to `Roms/PORTS/Games/<dir>/`, and its `App`-pointing shortcut is replaced by our own |
| `app` | optional, `app-to-ports` only: the zip's folder under `App/` when it isn't `dir`'s last part. For a second port of a game already in the catalog (miyoo64's Mario Kart 64 next to cacuracaptors'): its own `dir`, and the shortcut, picture and `GameName` go by `name`, so neither port touches the other's |
| `skip` | zip paths not installed (e.g. miyoo64's MinUI folders) |
| `dir` | the game folder on the card |
| `data` | what the player supplies: `required`, `required_any`, `optional`, `forbidden` (paths in `dir`, case-insensitive, `*` allowed), `rom`, `disc` |
| `shortcut` | for `app-to-ports`: `GameDataFile` and the script to run |
| `get` | instructions for the player |

## Onion's Ports list (checked in OnionUI/Onion and OnionUI/Ports-Collection, 2026-10-17)

- The list is `Roms/PORTS/Shortcuts` (`Emu/PORTS/config.json`: extensions `port|miyoocmd`,
  pictures from `Roms/PORTS/Imgs`). **Subfolders are categories** in the menu.
- Onion's "Import ports" (`Emu/PORTS/import.sh`) checks each shortcut: `GameDataFile` (or
  `GameExecutable`) found within 2 levels of `Roms/PORTS/Games/<GameDir>` → `.port` (shown),
  else `.notfound` (hidden). A shortcut with a real data file as `GameDataFile` therefore
  shows up only once the player has copied the data. Several of today's ports name their own
  binary there (Hexen II, Tomb Raider, SkiFree, Mario Kart 64), so they always show.
- Onion's Ports Collection uses these category folders: Action, Arcade, FPS, Platform,
  Puzzle games, Racing, Reflex, and series folders like `FPS - Doom Collection (PrBoom)`.
  MiyooMini-Ports also has rpg, shooter, simulation, engine, app.

Categories (Onion's names, plus RPG; confirmed by the owner 2026-10-17):

| Category | Games |
|---|---|
| Racing | Mario Kart 64, Mario Kart 64 (mr-finster), Diddy Kong Racing, F-Zero X |
| FPS | GoldenEye 007, Perfect Dark, Hexen II |
| Platform | Super Mario 64, Tomb Raider |
| Action | Ocarina of Time, Majora's Mask, Star Fox 64, Super Smash Bros. |
| RPG | Fallout, Fallout 2, Paper Mario, Mines of Moria, Monk Tower |
| Arcade | 3D Pinball Space Cadet, SkiFree, Tennis Vs Zombies |
| Strategy | Plants vs. Zombies |
| Simulation | Harvest Moon 64 (added with the owner's OK, 2026-10-20) |
| Puzzle games | Frozen Bubble, Panel Attack, Zuma Deluxe |

## miyoo64 outside App/ (checked in its sources, 2026-10-17)

miyoo64's `launch.sh` starts with `cd "$(dirname "$0")"` and the games find `data/` next to
their executable (`/proc/self/exe`; `App/<Game>/data` is only the fallback when that fails),
so a game folder can move to `Roms/PORTS/Games/<dir>/` as it is, saves included. Its own
shortcuts (`forward.sh`) hard-code `/mnt/SDCARD/App/<dir>`, so we write ours:
`GameDir="<dir>"`, `GameDataFile="rom.z64"`, then `exec sh .../launch.sh`. Its
`app_config.json` is harmless outside `App/`. Done on the owner's Miyoo on 2026-10-17
(the owner's `update_miyoo64.sh` script); categories applied there too (`docs/NOTES.md`).

[mmp]: https://github.com/Producdevity/MiyooMini-Ports
