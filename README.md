# portmaster-miyoo

Test repo for a port manager on the Miyoo Mini (OnionOS), PortMaster-like: it lists, installs
and updates native Miyoo ports, downloading each one from its author's GitHub releases. No
port zips are hosted here, only a catalog of where they are released (`catalog.json`).

Nothing runs yet. The plan (PortMaster-New analysis, steps) is in `miyoo/PORT_MANAGER.md` of
github.com/ksielick/redriver2-miyoo; the first step is the PC script that grows out of its
`miyoo/tools/update_miyoo64.sh`.

## Rules

- **Everything goes to Onion's Ports list**: game in `Roms/PORTS/Games/<dir>/`, shortcut in
  `Roms/PORTS/Shortcuts/<Category>/<Name>.port`, picture in `Roms/PORTS/Imgs/<Name>.png`.
  Nothing in `App/`, even for ports whose own zips install there (miyoo64).
- **Copyrighted files (ROMs, game data) are never downloaded or hosted**: the player copies
  them; every entry says where to get them legally and where they go (`get`).
- Files the player added, changed or created (data, saves, settings) are never overwritten or
  deleted; replaced files are backed up first.

## catalog.json

`sources`: other catalogs used for descriptions. [MiyooMini-Ports][mmp] (MIT, 66 entries,
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
| `repo` | `owner/name` on GitHub; the latest release is used |
| `asset` | regex for the release file; `null` = no releases yet |
| `layout` | `sdcard-root`: the zip mirrors the card (`Roms/PORTS/...`); `app-to-ports`: the zip has `App/<dir>/` (miyoo64), which goes to `Roms/PORTS/Games/<dir>/`, and its `App`-pointing shortcut is replaced by our own |
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
| Racing | Mario Kart 64, Diddy Kong Racing, F-Zero X, Carmageddon, Carmageddon Splat Pack, REDRIVER2 |
| FPS | GoldenEye 007, Perfect Dark, Hexen II |
| Platform | Super Mario 64, Tomb Raider |
| Action | Ocarina of Time, Star Fox 64, Super Smash Bros. |
| RPG | Fallout, Fallout 2, Paper Mario |
| Arcade | 3D Pinball Space Cadet, SkiFree |

## miyoo64 outside App/ (checked in its sources, 2026-10-17)

miyoo64's `launch.sh` starts with `cd "$(dirname "$0")"` and the games find `data/` next to
their executable (`/proc/self/exe`; `App/<Game>/data` is only the fallback when that fails),
so a game folder can move to `Roms/PORTS/Games/<dir>/` as it is, saves included. Its own
shortcuts (`forward.sh`) hard-code `/mnt/SDCARD/App/<dir>`, so we write ours:
`GameDir="<dir>"`, `GameDataFile="rom.z64"`, then `exec sh .../launch.sh`. Its
`app_config.json` is harmless outside `App/`. Done on the owner's Miyoo on 2026-10-17 (redriver2-miyoo's `update_miyoo64.sh`); categories applied there too.

[mmp]: https://github.com/Producdevity/MiyooMini-Ports
