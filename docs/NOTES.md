# Notes: decisions, findings, the card's state

Newest first within each section. Plan: `PLAN.md`; catalog format: `../README.md`.

## Owner's decisions (2026-10-17)

- A port manager like PortMaster, but downloading every port from its **author's GitHub
  releases**, not from one repo of zips. This repo is for tests; public since 2026-10-17 (history rewritten to the noreply e-mail first).
- **Ports list only**: every port in `Roms/PORTS/Games` + `Roms/PORTS/Shortcuts`; nothing in
  Onion's Apps (`App/`). Port entries already in Apps were to be removed (done, see below).
- **Thematic category folders** in `Roms/PORTS/Shortcuts`: Onion's names plus RPG (mapping
  in `../README.md`), confirmed and applied on the card.
- **Copyrighted files** (ROMs, game data): never downloaded or hosted; each entry gives the
  player instructions for adding them.
- **Carmageddon and Carmageddon Splat Pack are two separate games** (one repo, one binary,
  either installable alone).
- Ports to cover first: dethrace-miyoo (Carmageddon x2), REDRIVER2, miyoo64, all of
  cacuracaptors' ports; use Producdevity/MiyooMini-Ports as a source.
- Before changing the card: a **backup on the PC**.
- **The whole port manager is one C program** (owner, 2026-10-17: "cały w C", not a UI in C
  over shell scripts). The program itself is an app: **in Onion's Apps** (`App/PortManager`)
  and as `~Port manager` in the Ports list (owner) — the "never App/" rule is for the ports.
- Text pages scroll up/down instead of separate panels (owner).

## Emulator tests (2026-10-17, CI via GitHub Actions)

- **qemu-arm + emu_shim.c** ported from `dethrace-miyoo` into `miyoo/emu/`:
  - `src/gfx.c`: `gfx_flip()` now issues `ioctl(fb, FBIOPAN_DISPLAY, &var)` so `emu_shim.c` counts frames and can capture screenshots / dispatch input.
  - `src/util.c` and `src/pm.c`: `wget_bin()` dynamically resolves `wget` from `$PM_SD/.tmp_update/bin/wget`, `/mnt/SDCARD/.tmp_update/bin/wget`, or system `PATH`; `net_check` accepts `eth0` or `$PM_SD` when `wlan0` is not present (CI / emulator).
  - `src/build.sh`: uses `-D_FILE_OFFSET_BITS=64` so `readdir()` handles 64-bit inodes from 64-bit Linux hosts under `qemu-arm` without failing with `EOVERFLOW`. Supports `EMU=1` to compile `build/miyoo_emu/portmaster_emu` with `emu_shim.c`, `-Wl,--wrap=open,--wrap=open64,--wrap=ioctl,--wrap=read`, `-lm -lpthread`.
  - `emu_shim.c`: intercepts `read` on `/dev/input/event0` so when all scripted events are processed and the UI is idle waiting for input, it terminates cleanly (`_exit(0)`).
- **Mock SD card template** (`miyoo/emu/test_sd/`):
  - `.tmp_update/bin/`: host-forwarding wrappers for `wget`, `timeout` (translating `-t` flag for GNU coreutils), `nslookup`, `7z`.
  - Pre-installed test ports: Diddy Kong Racing (miyoo64 at 1.1.2, save data in `data/save.bin`), SkiFree (1.0.1, up to date), OpenLara (1.0.6, missing data).
- **Scenarios** (`miyoo/emu/scenarios/`):
  - `start_list.scn`: verifies initial UI and installed port list.
  - `scroll_details.scn`: opens details and scrolls down.
  - `catalog.scn`: displays full catalog by category.
  - `check_updates.scn`: queries GitHub API for updates.
  - `update_port.scn`: checks updates, confirms update of DKR 1.1.2 -> 1.1.3, verifies backup creation in `Backups/PortManager/` and updated `.pm_version`.
- **Workflow** (`.github/workflows/emu-test.yml`):
  - Automatically runs scenarios on ubuntu-24.04 with qemu-user, toolchain caching, and uploads artifacts (screenshots, logs, and compiled binaries).

## The C program (2026-10-17, on the owner's card, works)

- Tested by the owner on the device, 2026-10-17 (binary of commit 8516430):
  - Paper Mario and Diddy Kong Racing 1.1.2 -> 1.1.3 through the manager: files equal to the
    release (md5 checked), only launch.sh, full_resolution and the shortcut replaced (`dkr` is
    the same in both releases), the owner's box art kept, `data/` in the backup, the game
    starts.
  - Fallout (installed before the manager, no `.pm_version`): the check used to say "up to
    date" (bug: an empty version compared as no update). Now the details say "yours unknown",
    A installs the release over it; files equal to the release stay, only the version
    (`v1.2.0`, the repo's tag) is written.
  - The result page of a check had only B, while its text said A: A/Start now work there.
- Still unknown version (installed without the manager): Carmageddon, Hexen II, Tomb Raider,
  REDRIVER2, Mario Kart 64 (Y then A on their pages; REDRIVER2 and dethrace have no releases).
- On the card: `App/PortManager/` (portmaster,
  launch.sh, config.json, catalog.json) and `Roms/PORTS/Shortcuts/~Port manager.miyoocmd`.
  The shell version is removed from the card (`Games/PortManager`, the `~Port manager/`
  folder); its log is kept as `App/PortManager/log-shell-version.txt`. Ports cache cleared.

- `src/`: `main.c` (screens), `gfx.c` (fb0 + evdev + font8x8 x2: 38 columns), `pm.c` (the
  logic of the shell version, same rules), `util.c` (files, sha256, wget, run); vendored
  `cJSON` (MIT), `miniz` (MIT, zips), Brad Conte's `sha256` (public domain), `font8x8`
  (public domain). `src/build.sh` -> `onion/App/PortManager/portmaster` (static, ~550 KB).
  Only HTTPS goes outside: Onion's `wget` (no TLS library in the program yet); 7z/tar
  releases also through Onion's tools; nslookup/timeout for the DNS test.
- On the card: `App/PortManager/` (`portmaster`, `launch.sh`, `config.json` for Apps,
  `catalog.json`, `log.txt`) and `Roms/PORTS/Shortcuts/~Port manager.miyoocmd` -> its
  launch.sh. Work files and lock as before (`/mnt/SDCARD/.pm_work`).
- Screens: the installed ports by name (status on the right: version, `old>new` in purple,
  "files missing" in amber); A details (scrollable; Y there checks only that port's repo,
  no catalog refresh, any port incl. not installed; A there updates when there's an update),
  X all ports (by category, headings), Y check for updates (progress page with the log lines
  as they come), Start update all (asks first), B back/exit. Held buttons repeat only for
  Up/Down/Left/Right/L1/R1.
- Manifest: sha256 now (the shell version's md5 manifests were never written: only dry runs).
- Deployed (above) but never run yet. No host compiler or WSL on the PC. To deploy again:
  copy `onion/App/PortManager` (+ the root `catalog.json`) and the .miyoocmd over SSH
  (`mscp`, one file per call; App/ isn't on the SMB share), then clear
  `Shortcuts_cache*.db`.
- Emulator tests: see "Emulator tests" above (from dethrace-miyoo's `miyoo/emu/`).

## The shell version (first, 2026-10-17; replaced by the C program, removed from the card)

- `onion/` mirrors the card: `Roms/PORTS/Games/PortManager/` (`pm.sh`, `menu.sh`, plus
  `catalog.json` copied from the repo root) and `Roms/PORTS/Shortcuts/~Port manager/~Refresh
  list.miyoocmd` (Onion runs `.miyoocmd` files as scripts; it builds the rest of the menu
  folder). Deployed by copying these over SMB, then `menu.sh build`.
- `pm.sh check` / `pm.sh update [id...]` (`PM_DRY=1` = plan only); refreshes `catalog.json`
  from raw.githubusercontent.com on each run (so push catalog changes before testing);
  working files in `/mnt/SDCARD/.pm_work`, backups in `Backups/PortManager/`, log in
  `PortManager/log.txt`; version in `<game>/.pm_version` (also `.miyoo64_version` when there),
  installed files' md5 in `<game>/.pm_manifest`.
- The menu (owner, 2026-10-17: nothing checked at start, the installed ports listed, update
  checks for installed ports only, the whole catalog on request) is **a folder in Onion's
  Ports list**, `Shortcuts/~Port manager/`, built by `menu.sh build` (no network, ~4 s): one
  `.miyoocmd` per installed port, "<Name> (<version>)", which shows its details and
  game-file instructions (`pm.sh info`); `~Check for updates` (`pm.sh check`, network,
  catalog refreshed; afterwards `~Update all (N)` and `~Update <Name> <old> to <new>` appear,
  choosing one is the confirmation); `~Refresh list`; the subfolder `~All ports in the
  catalog` ("<Category> - <Name> (status)"). MainUI does the browsing and B; file names
  can't hold `: / \ * ? " < > |` (FAT). After a rebuild the `*_cache*.db` list caches go.
- Network test before check/update (`net_check`, ~1 s; owner asked for it after a "no
  connection?" list during an internet outage): Wi-Fi address on wlan0 (`ip -4 addr`), DNS
  (`timeout -t 5 nslookup`, once more on failure — the home router's DNS drops out now and
  then; without the timeout one lookup took 26 s), GitHub's `rate_limit` (free; 60 requests
  an hour without a token, each check takes one per repo = 7 now). One sentence comes back
  as `NONET <why>` and the menu shows only that. Release fetches are retried once, wget times
  out after 15 s. A run killed mid-way (e.g. its output cut by `head`) used to leave
  `.pm_work/lock`; the lock now holds the pid and a dead one is taken over.
- Texts (port details, check/update results) show in **pmview** (`src/pmview.c`, built by
  `src/build.sh` with redriver2-miyoo's Arm toolchain, static, 342 KB): one scrollable page
  (owner: scroll up/down rather than separate panels), drawn straight to fb0's shown page
  (turned 180°) with font8x8 (public domain) at x2 = 38 columns, 17 lines; Up/Down a line,
  Left/Right or L1/R1 a page, A/B/Menu closes; scroll bar and "first-last/total". Tested on
  the device with MainUI paused, keys from `sendkeys` and screenshots by redriver2-miyoo's
  `miyoo/tools/mshot.sh`. menu.sh falls back to infoPanel panels (42 columns, 5 lines:
  infoPanel doesn't wrap and keeps 255 characters) when pmview is missing or fails.
- Why not a list dialog: Onion's `prompt` on this card **always exits with 136 (signal 8,
  SIGFPE)** — tested over SSH with MainUI paused (`kill -STOP`) and keys sent by Onion's
  `sendkeys <code> <value>` (A=57, B=29, Down=108): A, Down+A, B, with/without `-m`, 2 or 19
  items: all 136, so the choice can't be read (the first menu then opened an empty entry and
  B couldn't leave it). Careful with such tests: a run that hangs leaves MainUI stopped
  (`kill -CONT` it) and keys possibly held (`sendkeys 57 0 29 0 108 0`). `infoPanel`
  messages are cut at 255 characters, so longer texts go in several panels.
- Tested over SSH: `check` right for all installed entries: 98 s at first (jq takes ~0.5 s to
  start on the Miyoo and ran ~180 times), 8.6 s since jq runs once for the catalog and once per
  release, releases fetched in parallel (wget ~0.8 s each); dry run of 1.1.3 (all 9: 48 s with
  the downloads): 3 files each
  (`launch.sh`, the new `full_resolution`, our shortcut regenerated with the release's
  `FullResolution=1`), the user's pictures kept. Fixed: release info was cached across runs.
- PC backup before the first real update: `G:\Emulacja\redriver2-miyoo\backup\20261017-before-pm-1.1.3\miyoo-sdcard.tar`
  (md5 20bc3e88431424f1b50aad1b0746dbeb: Shortcuts, the 9 miyoo64 games' `data/`,
  `launch.sh`, `.miyoo64_version`).
- Next: the owner runs "~Port manager" from the Ports list (1.1.2 -> 1.1.3); then
  install of new ports, curl + CA bundle.

## The owner's card (2026-10-17)

- Ports list (`Roms/PORTS/Shortcuts/`), ours and the authors' ports:
  - Racing: Diddy Kong Racing, F-Zero X (miyoo64), Mario Kart 64 (mk64-miyoomini),
    Carmageddon, Carmageddon Splat Pack (dethrace-miyoo), REDRIVER2;
  - FPS: GoldenEye 007, Perfect Dark (miyoo64), Hexen II;
  - Platform: Super Mario 64 (miyoo64), Tomb Raider (OpenLara);
  - Action: Ocarina of Time, Star Fox 64, Super Smash Bros. (miyoo64);
  - RPG: Paper Mario (miyoo64), Fallout, Fallout 2.
  The same folders also hold Onion's Ports Collection games (Action, FPS, FPS - ...
  Collection, Platform, Puzzle games, Racing, Reflex). Not installed: 3D Pinball, SkiFree.
  Leftovers of Onion's import, left alone: `Action/Bomberman (mr.boom)` and `Action/Pong
  (Gong)` exist as both `.port` and `.notfound`.
- miyoo64 (all at 1.1.2): `Roms/PORTS/Games/<Name>/` for DiddyKongRacing, FZeroX, GoldenEye,
  OcarinaOfTime, PaperMario, PerfectDark, StarFox64, SuperMario64, SuperSmashBros; ROM as
  `rom.z64`, saves in `data/`. Updated with redriver2-miyoo's `miyoo/tools/update_miyoo64.sh`,
  which since 2026-10-17 installs there (never `App/`), writes the shortcut into the category
  folder when the game has none, and moves any game still in `App/`.
- `App/` now holds only Onion's own apps (240pSuite, AdvanceMENU, Clock, Tweaks, ...).
- The user's pictures in `Roms/PORTS/Imgs` (box covers) differ from the releases' and are
  kept by the updater.

### The move out of App/ (2026-10-17)

1. Backup to the PC: `G:\Emulacja\redriver2-miyoo\backup\20261017-before-ports\miyoo-sdcard.tar`
   (385 MB, md5 a51f15ee116d25a278619c10bae3afe8, checked after the copy): the 9
   `App/<miyoo64 game>/` folders (saves, ROMs), `Roms/PORTS/Shortcuts`, `Roms/PORTS/Imgs`,
   the 8 MinUI folders `Roms/<Game (X)>/`, `Roms/recentlist.json`, `Roms/favourite.json`.
   Made as a tar on the card, copied over SMB (68 s), deleted from the card.
2. `update_miyoo64.sh 1.1.2`: each game `App/<Name>` → `Roms/PORTS/Games/<Name>` (saves'
   sizes the same before and after), its `config.json` (the Apps entry) and old shortcut
   (pointing to `App/`) to `Backups/miyoo64/20261017-112542-before-1.1.2/` on the card, new
   shortcut in the category folder, Onion's recent list pointed at it.
3. The other 8 shortcuts from the Shortcuts root into categories, recent list updated (all 11
   entries checked: the files exist); the 8 MinUI folders `Roms/<Game (X)>/` (only empty
   `.pak` launchers, useless on Onion) moved to the same card backup; list cache reset.
4. Not yet checked by the owner: launching the moved games from the Ports list and their saves.

## Onion (checked in OnionUI/Onion, OnionUI/Ports-Collection and on the device, 2026-10-17)

- Ports list: `Emu/PORTS/config.json` — `rompath ../../Roms/PORTS/Shortcuts`, `imgpath
  ../../Roms/PORTS/Imgs`, extensions `port|miyoocmd`. Subfolders show as categories.
  `Emu/PORTS/launch.sh` does `cd "$(dirname "$1")"`, `chmod +x`, runs the `.port` itself.
- "Import ports" (`Emu/PORTS/import.sh`): for every `.port`/`.notfound` takes `GameDir`
  (else `RomDir`) under `Roms/PORTS/Games/`, and `GameDataFile` (else `GameExecutable`, else
  `RomFile`, else `<Core>_libretro.so`); `find <GameDir> -maxdepth 2 -iname <file>` found →
  `.port`, else `.notfound` (hidden). Ports Collection launchers call
  `/mnt/SDCARD/Emu/PORTS/launch_standalone.sh "$GameName" "$GameDir" "$GameExecutable" ...`.
- Ports Collection category folders: Action, Arcade, FPS, Platform, Puzzle games, Racing,
  Reflex, and series folders (`FPS - Doom Collection (PrBoom)`, `FPS - Duke Collection
  (eduke32)`, `FPS - Quake Collection (TyrQuake)`, `FPS - Wolfenstein Collection (ECWolf)`).
  Licensed data instructions there: `_required_files.txt` in each game folder.
- `/mnt/SDCARD/.tmp_update/script/reset_list.sh <dir>` resets a list's cursor and cache, but
  when `/tmp/state.json` doesn't exist it creates a 1-byte one: delete it afterwards. The
  Ports cache file was `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`.
- Recent list `Roms/recentlist.json`: one JSON object per line, `"rompath":
  "/mnt/SDCARD/Emu/PORTS/../../Roms/PORTS/Shortcuts/<path>.port"` — moving a shortcut needs
  this path changed (`favourite.json` has the same format).
- Tools on the device (`/mnt/SDCARD/.tmp_update/bin/`): `wget` (HTTPS to api.github.com
  works), `jq`, `unzip`, `7z`, `sqlite3`, `openssl`, `infoPanel`, `prompt`; `curl` fails on
  HTTPS (no CA bundle, error 60). No Python.
- Private repos from the device (checked 2026-10-17, no token tried yet): Onion's `wget` is a
  17 KB uclient-fetch-like tool — options `-q -O -P -T`, `--user/--password` (HTTP Basic),
  `--ca-certificate`, `--no-check-certificate`; **no custom headers**. There is no CA store
  (`/etc/ssl/certs` missing), yet it fetched api.github.com: it probably doesn't verify the
  server (to check before sending a token through it). A private repo answers 404 without
  auth. GitHub's API takes Basic auth with the token as password, so wget could list private
  releases, but downloading a private release asset needs `Accept: application/octet-stream`
  on `/repos/{o}/{r}/releases/assets/{id}` (then a redirect to a signed URL), which wget can't
  send. **Plan: curl with a CA bundle shipped in the app** (`--cacert cacert.pem`, ~220 KB):
  `-H "Authorization: Bearer <token>"`, `-H "Accept: application/octet-stream"`, `-L` (curl
  doesn't forward the token to the redirect's other host). Token on the card: readable by
  anyone on the LAN (SSH root without password, open SMB share), so a fine-grained token,
  only the needed repos, Contents read-only, with an expiry — or public release repos.

## miyoo64 (mr-finster/miyoo64, checked in its sources, 2026-10-17)

- Release zips: `App/<Name>/` (binary, `recipes.bin`, `launch.sh`, `config.json`, `icon.png`),
  Onion shortcut + picture under `Roms/PORTS/`, MinUI folders (`Emus/`, `Apps/`,
  `Roms/<Game (X)>/`); since 1.1.1 also `Miyoo64-all-miyoo.zip` (all games, skipped).
  Assets have GitHub's sha256 `digest`.
- Movable: `launch.sh` does `cd "$(dirname "$0")"`, the games find `data/` next to their
  executable (`/proc/self/exe`; `/mnt/SDCARD/App/<Game>/data` only as a fallback). Its
  shortcuts (`miyoo/templates/forward.sh`) hard-code `/mnt/SDCARD/App/<Name>` and name
  `launch.sh` as `GameDataFile`, so ours replace them (`GameDataFile="rom.z64"`).
- Its MarioKart64 is a different port from mk64-miyoomini (the one on the card); both would
  use `Roms/PORTS/Games/MarioKart64`: don't install it.

## cacuracaptors' ports (checked 2026-10-17)

All are forks with releases (one zip each, mirroring the card: `Roms/PORTS/Games/<dir>/`,
`Shortcuts/<Name>.port` in the root, `Imgs/<Name>.png`); version in the asset name; **no
`digest`** on the assets; no game data in the zips (several bundle their own SDL2/GLES libs).

| Port | Repo | Latest | Game folder | Player supplies |
|---|---|---|---|---|
| Hexen II | uhexen2-miyoomini | v1.0.0 | Hexen2 | GOG `data1/` (+ music) |
| SkiFree | skifree-miyoomini | v1.0.1 | SkiFree | nothing |
| Tomb Raider | OpenLara-miyoomini | v1.0.6 | OpenLara | GOG `GAME.GOG` (+ mp3s); first start unpacks it to `DATA/`, `FMV/` and deletes it |
| 3D Pinball Space Cadet | spacecadetpinball-miyoomini | v1.4.0 | Pinball | `PINBALL.DAT`, `FONT.DAT`, `SOUND*.WAV` (+ `PINBALL.MID`) |
| Fallout | fallout1-ce-miyoomini | v1.2.0 | Fallout | `MASTER.DAT`, `CRITTER.DAT`, `data/`; no PC `fallout.cfg` |
| Fallout 2 | fallout2-ce-miyoomini | v1.2.0 | Fallout 2 | `master.dat`, `critter.dat`, `data/`; no PC `fallout2.cfg` |
| Mario Kart 64 | mk64-miyoomini | v1.1.5 | MarioKart64 | Mario Kart 64 (USA) ROM |

Their `GameDataFile`: Fallout/Fallout 2 `master.dat`, Pinball `PINBALL.DAT` (real checks);
Hexen II, Tomb Raider, SkiFree, Mario Kart 64 name the binary (always "found").
`MiyooMini-Ports` in their account is a fork of Producdevity's catalog (they add their
entries there).

## MiyooMini-Ports (Producdevity, MIT, checked 2026-10-17)

`ports.json` (+ `ports.schema.json`, `porters.json`, a website built from them): 66 entries,
fields `name`, `categories` (app, arcade, engine, fps, platform, puzzle, racing, reflex, rpg,
shooter, simulation), `status` (playable 51, experimental 11, prerelease 3, source-only 1),
`assets` (free 43, owned 23), `porter`, `upstream` (the `/releases` page), `image`, `notes`.
All upstreams are on GitHub. Latest releases: ~55 have one, ~10 none (404). Asset formats:
mostly zip, also 7z, tar, tar.gz, ipk, AppImage; many releases ship builds for other systems
(spruce, Allium, CrossMix, PortMaster, Android) beside the Onion one, some with `.sha256`
files. 34 entries are apps, not games (they install into Onion's Apps by nature).

## Our own ports

- REDRIVER2 (ksielick/redriver2-miyoo, private): `Roms/PORTS/Games/REDRIVER2/`, the discs
  (Driver 2 USA, CHD or BIN/CUE) from `Roms/PS`; no GitHub releases yet.
- dethrace-miyoo (ksielick/dethrace-miyoo, private, `G:\Emulacja\dethrace-miyoo`):
  `Roms/PORTS/Games/Carmageddon/`, `Roms/PORTS/Games/SplatPack/`; data from the GOG Max Pack
  via `miyoo/install/install_gog.bat` on the PC, or the Carmageddon demo; no releases yet.
