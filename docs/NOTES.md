# Notes: decisions, findings, the card's state

Newest first within each section. Plan: `PLAN.md`; catalog format: `CATALOG.md`.

## Owner's decisions (2026-10-17)

- A port manager like PortMaster, but downloading every port from its **author's GitHub
  releases**, not from one repo of zips. This repo is for tests; public since 2026-10-17 (history rewritten to the noreply e-mail first).
- **Ports list only**: every port in `Roms/PORTS/Games` + `Roms/PORTS/Shortcuts`; nothing in
  Onion's Apps (`App/`). Port entries already in Apps were to be removed (done, see below).
- **Thematic category folders** in `Roms/PORTS/Shortcuts`: Onion's names plus RPG (mapping
  in `CATALOG.md`), confirmed and applied on the card.
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
  - `src/build.sh`: uses `-D_FILE_OFFSET_BITS=64` so `readdir()` handles 64-bit inodes from 64-bit Linux hosts under `qemu-arm` without failing with `EOVERFLOW`. Supports `EMU=1` to compile `build/miyoo_emu/portmanager_emu` with `emu_shim.c`, `-Wl,--wrap=open,--wrap=open64,--wrap=ioctl,--wrap=read`, `-lm -lpthread`.
  - `emu_shim.c`: intercepts `read` on `/dev/input/event0` so when all scripted events are processed and the UI is idle waiting for input, it terminates cleanly (`_exit(0)`).
- **Mock SD card template** (`miyoo/emu/test_sd/`):
  - `.tmp_update/bin/`: host-forwarding wrappers for `wget`, `timeout` (translating `-t` flag for GNU coreutils), `nslookup`, `7z`.
  - Pre-installed test ports: Diddy Kong Racing (miyoo64 at 1.1.2, save data in `data/save.bin`), SkiFree (1.0.1, up to date), OpenLara (1.0.6, missing data), Fallout (unknown version, no `.pm_version`).
- **Scenarios** (`miyoo/emu/scenarios/`):
  - `start_list.scn`: verifies initial UI and installed port list.
  - `scroll_details.scn`: opens details and scrolls down.
  - `catalog.scn`: displays full catalog by category.
  - `check_updates.scn`: queries GitHub API for updates.
  - `update_port.scn`: checks updates, confirms update with direct `Start` on check results screen (DKR 1.1.2 -> 1.1.3), verifies backup and `.pm_version`.
  - `fallout_install.scn`: tests Fallout unknown version flow (details, Y, `A: install` directly from check results screen, confirm, writes `.pm_version` `v1.2.0`).
  - `remove_delete_all.scn`: tests uninstalling a port and deleting all files/folder (SkiFree).
  - `remove_keep_data.scn`: tests uninstalling a port while keeping game data, ROMs, and saves (Fallout: removes shortcut and program files, retains `MASTER.DAT`, `CRITTER.DAT`, `data/`).
  - Added 2026-10-19 (owner: tests for every feature), 22 in all, all passing:
    `update_single` (details: check, update, installed files; data kept, backup before
    copying), `restore_backup` (files and version back), `delete_backup`, `older_release`
    (GitHub list, second release), `install_new_port` (F-Zero X from all ports, no backup),
    `remove_keep_data_rom` (miyoo64: `rom.z64`, `data/` stay), `remove_cancel` (each step),
    `self_update` (old Port Manager updated first, backup, restart with --check),
    `self_update_later`, `start_check` (`--check`), `offline`, `broken_catalog`,
    `already_running`, `kept_shortcut` (the player's own shortcut kept).
  - Runner directives besides `press`/`shots`/...: `file <path> <text>` and `rm <path>` set
    the card up before the run (`app/...` = Port Manager's folder, `\n` in text, `{pid}` = a
    live pid), `offline` (wget/nslookup stubs fail), `assert_exists`/`assert_not_exists`
    (`*` globs), `assert_contains`/`assert_not_contains`, `assert_log`/`assert_log_not`/
    `assert_log_order a b c` on the log; quoted arguments may hold spaces. The test card is
    made 0777 like FAT.
  - Under the emulator the restart after a self-update is skipped (the downloaded binary is
    the device build, without the shim).
- **Workflow** (`.github/workflows/emu-test.yml`):
  - Automatically runs all scenarios on ubuntu-24.04 with qemu-user, toolchain caching, and uploads artifacts (screenshots, logs, and compiled binaries).

## ASCII Progress Bar & Download Size Display (2026-10-17 22:12)

- **Download size line in check results**:
  - `src/pm.c`: `pm_check()` now extracts the release asset's byte size (`p->asset_size`) and filename (`p->asset_name`) using the refactored `find_asset()` helper.
  - `src/main.c`: `do_check()` displays a dedicated line with the file size under the version update text:
    `Star Fox 64: 1.1.2 -> 1.1.3`
    `Download size: 1.8 MB`
  - In update all summary, size is shown next to each port: `  <Name>: <old> -> <new> (X.X MB)`.
- **ASCII download progress bar**:
  - Replaced graphical rect bar with a retro terminal ASCII bar:
    `[=============>          ]  54%` (rendered in `C_GOOD` green)
    `  1.0 / 1.8 MB` (in `C_TEXT` white)
  - Fits cleanly into the monospace font8x8 layout above the footer without overlapping.

## Details View Primary Action Button (2026-10-17 22:06)

- **`A` as primary action**:
  - In `details()`, `A` is now always mapped: triggers `do_check(p)` when unchecked or up to date (`A:check`), and triggers `do_update(p)` when an update or install is available (`A:update` / `A:install`).
  - Seamless `A -> A -> A` workflow: press `A` on the main list to open details, press `A` to check updates, and press `A` on the results page to install.
  - `Y` remains as a secondary check shortcut (`Y:check`). Footer hints are compact and fit within the screen width without clipping.

## Download Progress Bar & Auto-scroll (2026-10-17 21:55)

- **Download progress bar**:
  - `src/util.c`: `fetch()` now accepts `total_bytes` and runs `wget` in the background with `run_bg()`, polling `file_size(part)` every 60ms with non-blocking `waitpid(..., WNOHANG)`.
  - `src/main.c`: `download_progress()` callback displays a graphical progress bar (`C_SEL` container, `C_GOOD` fill) above the footer, with percentage and MB downloaded (`XXX%  X.X / X.X MB`).
  - Progress bar smoothly updates at 60ms intervals without freezing the screen.
- **Auto-scroll to bottom**:
  - `progress_draw()` maintains `top = progress.n > rows ? progress.n - rows : 0` so new messages automatically scroll into view.
  - `view_at()` supports `start_top = -1` (used by `progress_end()`), so when checking or updating finishes, the view starts scrolled all the way to the bottom displaying the final status (e.g. "Done: 1 updated.") without manual scrolling.

## Check Progress Clarity & Tools (2026-10-17 21:50)

- **Muted `scan: %d/%d` from UI**: `pm_scan()` now logs `--- scan: %d/%d ports installed` so it only writes to `log.txt` and is filtered out by `progress_sink()`.
- **Descriptive check progress**: `do_check()` now displays `Checking <Name>...` when checking a single port (e.g. `Checking Fallout 2...`), or `Checking installed ports...` when checking all ports.
- **Automated scripts**: `tools/deploy_to_miyoo.ps1` and `tools/screenshot_miyoo.ps1` added and documented in `AGENTS.md` and `CLAUDE.md`. Deployed to Miyoo.

## Deployment to Miyoo (2026-10-17 21:25)

- **Backup**: `backup/20261017-before-deploy/portmanager-backup.tar` (618 KB tar of `/mnt/SDCARD/App/PortManager`) and `~Port manager.miyoocmd` backed up on PC before updating.
- **Deployed files**:
  - `App/PortManager/`: updated `portmanager` (compiled C binary with uninstall support and portmanager-miyoo repo URL), `launch.sh`, `config.json`, `icon.png` (74x74 OnionOS transparent sticker), `catalog.json` (with PvZ and Zuma Deluxe). Removed old `portmaster` binary.
  - `Roms/PORTS/Shortcuts/~Port manager.miyoocmd` and `Roms/PORTS/Imgs/~Port manager.png`: updated over SMB.
  - Cleared `Roms/PORTS/Shortcuts/Shortcuts_cache*.db` to force OnionOS list reload.

## Port Uninstallation & OnionOS Icon (2026-10-17)

- **Uninstallation (`X` in port details)**:
  - First prompt: `Remove <Name>? / This will remove the port and its shortcut.` (`A: yes   B: no`).
  - Second prompt: `Keep game data in the folder? / (<dir>) / e.g. ROMs, saves, game data files.` (`A: keep   B: delete all`).
  - `keep_data = 0`: completely removes the game directory and shortcuts.
  - `keep_data = 1`: deletes executables (ELF binaries and scripts starting with `#!`), shared libraries (`.so`), metadata (`.pm_version`, `.miyoo64_version`, `.pm_manifest`), documentation, and shortcuts; preserves ROMs, save files, configurations, catalog-required data files, and `data/` subdirectories.
  - Cleans up empty parent shortcut directories and OnionOS cache DBs so the Ports list updates instantly.
- **OnionOS Icon**:
  - Pixel-perfect OnionOS squircle icon created with "PM" letters, cyan accent outline, and "PORT MANAGER" pill in `onion/App/PortManager/icon.png` (referenced in `config.json`) and `onion/Roms/PORTS/Imgs/~Port manager.png`.
- **Miyoo64 Exact ROM Requirements**:
  - Catalog updated with exact names, USA release versions, and SHA-1 checksums for all 10 supported N64 ports (Diddy Kong Racing, F-Zero X, GoldenEye 007, Ocarina of Time, Paper Mario, Perfect Dark, Star Fox 64, Super Mario 64, Super Smash Bros, Mario Kart 64).
- **Plants vs. Zombies (Fnava1/pvz-portable-miyoo-)**:
  - Added to catalog under `Strategy` category. Release `v1.0.1` (`PvZ_MiyooMiniPlus_v1.0.1.zip`).
  - Layout: `sdcard-root` (`Roms/PORTS/Games/Plants vs. Zombies`, `Roms/PORTS/Shortcuts/Strategy/Plants vs. Zombies.port`, `Roms/PORTS/Imgs/Plants vs. Zombies.png`).
  - Required data: `main.pak` (or converted `main_mmp.pak`). Saves backed up in `userdata/`.
- **Zuma Deluxe (Fnava1/zuma-deluxe-miyoo)**:
  - Added to catalog under `Puzzle games` category (`asset: null` pending author's tagged release).
  - Layout: `sdcard-root` (`Roms/PORTS/Games/Zuma Deluxe`, `Roms/PORTS/Shortcuts/Puzzle games/Zuma Deluxe.port`, `Roms/PORTS/Imgs/Zuma Deluxe.png`).
  - Required data: `main.pak` or loose folders (`levels/`, `images/`, `sounds/`, `fonts/`), optional `music/zuma.it`. Saves backed up in `userdata/`.

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
- On the card: `App/PortManager/` (portmanager,
  launch.sh, config.json, catalog.json) and `Roms/PORTS/Shortcuts/~Port manager.miyoocmd`.
  The shell version is removed from the card (`Games/PortManager`, the `~Port manager/`
  folder); its log is kept as `App/PortManager/log-shell-version.txt`. Ports cache cleared.

- `src/`: `main.c` (screens), `gfx.c` (fb0 + evdev + font8x8 x2: 38 columns), `pm.c` (the
  logic of the shell version, same rules), `util.c` (files, sha256, wget, run); vendored
  `cJSON` (MIT), `miniz` (MIT, zips), Brad Conte's `sha256` (public domain), `font8x8`
  (public domain). `src/build.sh` -> `onion/App/PortManager/portmanager` (static, ~550 KB).
  Only HTTPS goes outside: Onion's `wget` (no TLS library in the program yet); 7z/tar
  releases also through Onion's tools; nslookup/timeout for the DNS test.
- On the card: `App/PortManager/` (`portmanager`, `launch.sh`, `config.json` for Apps,
  `catalog.json`, `log.txt`) and `Roms/PORTS/Shortcuts/~Port manager.miyoocmd` -> its
  launch.sh. Work files and lock as before (`/mnt/SDCARD/.pm_work`).
- Screens: the installed ports by name (status on the right: version, `old>new` in purple,
  "files missing" in amber); A details (scrollable; category displayed in the header on the
  right; color-coded layout: Installed version, Latest version highlighted with amber for
  not-checked/updates or green for up-to-date, Location on SD card, ROM name & Game files status,
  Source repo, and Instructions; A checks when unchecked or updates/installs when available;
  Y also checks; X uninstalls), X all ports (by category, headings), Y check for updates
  (progress page with download size in MB/KB), Start update all,
  B back/exit. Held buttons repeat only for Up/Down/Left/Right/L1/R1.
- Inline download progress bar: during asset downloads, an ASCII progress bar (`[====>  ]  54%  1.0/1.8 MB`)
  appears as an inline line directly under the download announcement in the log, updates in place,
  and stays permanently at 100% in the scrollable log history after download completes.
- Built game data detection: added `built` array in `catalog.json` under `data` (e.g. Mario Kart 64:
  `dc_data/setup.ok`, `dc_data/romdata.bin`). If the original ROM (`rom.z64`) was unpacked and deleted
  by the port on first launch, `pm_scan()` verifies the built files and marks game files as `OK`
  rather than `missing`. `dc_data/` is also protected from removal during clean/updates.
- Inline color escapes in `gfx.h` / `gfx.c` (`CLR_TEXT`, `CLR_DIM`, `CLR_ACCENT`, `CLR_GOOD`,
  `CLR_WARN`, `CLR_BAD` as `\x01`..`\x06`), handled transparently by `gfx_text()` without
  advancing visible column counters; `lines_add()` tracks active color across word-wrapped lines.
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
- Update order (2026-10-19, owner's request): download -> extract -> work out which files
  change (unchanged ones and the player's own edits skipped) -> backup tar in
  `Backups/PortManager/` (no progress bar; if `tar` fails the update stops before any file
  is written) -> copy with a bar counting only the files actually replaced. Before, the tar
  was made after copying and the bar counted unchanged files too (0% -> 100% jump). On the
  progress page a log line arriving while a bar runs goes above the bar (fixed a stale
  `0/18` bar left behind by "saved ... to the backup").
- Progress screen (2026-10-19, owner: full details in the log, a simple screen, one bar):
  the port's name once (purple), then only lines worth reading (warnings, "Backup saved to Backups/PortManager",
  "v1 -> v2, N file(s) replaced"); the `<name>: ` prefix is cut on screen only. One status
  place under the lines: step label (`1/4 Downloading`, `2/4 Extracting`, `3/4 Backup`,
  `4/4 Copying`) and its bar, replaced step by step (`on_stage`, `say_log`/`say_screen` in
  `util.c`). Update all: `k/n` in the header's right corner.
- Self-update: compares the latest GitHub release tag of ksielick/portmanager-miyoo with
  `App/PortManager/.pm_version` (fallback `PM_VERSION`); the release workflow now writes the
  tag into `.pm_version` in the zip. v1.1.0 is the first release (the card still says 1.0.0,
  so it should offer the update: the first real test of self-update).
- Self-update first (2026-10-19, owner): Y checks Port Manager's own release alone
  (`pm_check_self`) before the ports; a newer one -> "A: update now / B: later"; after the
  update it restarts with `--check` and checks the ports at once. `pm_check` no longer
  fetches Port Manager's release. Releasing: bump `PM_VERSION` in `src/pm.h` to the tag first
  (the release workflow fails otherwise; with a mismatch the emulator scenarios would meet
  the self-update question).
- Check for updates: the status line shows the port whose release it is waiting for
  ("Checking Diddy Kong Racing", bar "2/5 ports"; "Updating the catalog" before). Releases
  go up by 0.0.1 (owner, 2026-10-19).
- Owner's decisions, 2026-10-19 (afternoon):
  - **Releases:** v1.2.0 is the fresh start (v1.1.x releases and tags deleted); next v1.2.1, v1.2.2, ...
    An old release is deleted only after the new one is out.
  - **Release notes in plain language for players:** `docs/releases/<tag>.md`, which the release
    workflow publishes instead of the commit list. Changes are also reported to the owner in
    player terms.
  - **GitHub page:** README for players (install, making games appear, the copyright line).
    Developer notes are in `CATALOG.md`. Old workflow runs deleted; the two workflows stay.
- Self-update tested on the device by the owner, 2026-10-19: 1.0.0 -> v1.1.0 -> v1.1.1 over
  Wi-Fi, "works very well".
- F-Zero X ROM and saves lost (owner, 2026-10-19): the build on the card on 2026-10-17
  evening (efd46af) took any file with an exec bit for a program file, checked before the data
  rules; on FAT every file is rwxrwxrwx, so "remove, keep data" deleted all 12 files (3 program,
  `rom.z64`, 8 in `data/`). The exec-bit check went in 91dff62; now the catalog's data rules
  also come before any code guess, and the emulator makes the test card 0777 like FAT.
  Restored: `rom.z64` from `G:\Emulacja\n64\F-Zero X (USA).zip` (SHA-1 checked), `data/` from
  `G:\Emulacja\redriver2-miyoo\backup\20261017-before-pm-1.1.3\miyoo-sdcard.tar` (saves of
  2026-10-16 20:35); the folder before that in `backup/20261019-before-fzerox-restore/`.
  Hexen II, Carmageddon, Tomb Raider (also removed keeping data) still have their data.
- Emulator scenarios `remove_*`: fixed quit frames dropped (the removal page draws more
  frames now and was cut off); they end when the scripted keys run out.
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
