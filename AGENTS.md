# AGENTS.md - Instructions for AI Agents in portmanager-miyoo

## Building Port Manager
To cross-compile the binary for the Miyoo Mini Plus:
```powershell
& "G:\Git\bin\sh.exe" -c "cd src && ./build.sh"
```
The cross-compiler is located at `G:\Emulacja\redriver2-miyoo\deps\arm-gnu-toolchain-13.3.rel1-mingw-w64-i686-arm-none-linux-gnueabihf\bin\arm-none-linux-gnueabihf-gcc.exe`. The build outputs `onion/App/PortManager/portmanager` (~550 KB static binary).

## Deployment to Miyoo Mini Plus
To deploy the latest version of Port Manager to the physical Miyoo device over LAN/Wi-Fi:
```powershell
powershell -ExecutionPolicy Bypass -File tools/deploy_to_miyoo.ps1
```
This automated script:
1. Verifies connectivity to the Miyoo Mini Plus at `192.168.0.246`.
2. Creates an automatic verified PC backup in `backup/<timestamp>-before-deploy/portmanager-backup.tar`.
3. Copies `~Port Manager.miyoocmd` and `~Port Manager.png` to `\\192.168.0.246\Roms` (or SSH fallback).
4. Streams `onion/App/PortManager/` tar over SSH into `/mnt/SDCARD/App/PortManager/`, sets `chmod +x` on binaries and scripts, and deletes any old `portmaster` binary.
5. Clears OnionOS list cache (`/mnt/SDCARD/Roms/PORTS/Shortcuts/*cache*.db`).

## Capturing Screenshot from Miyoo Mini Plus
To capture a real-time framebuffer screenshot from the device:
```powershell
powershell -ExecutionPolicy Bypass -File tools/screenshot_miyoo.ps1 [out.png]
```
This dumps `/dev/fb0`, queries the pan offset, and converts to PNG using `ffmpeg`.

## UI Conventions & Architecture
1. **Inline Color Escapes**:
   - `src/gfx.h` defines `CLR_TEXT`, `CLR_DIM`, `CLR_ACCENT`, `CLR_GOOD`, `CLR_WARN`, `CLR_BAD` as control codes `\x01`..`\x06`.
   - `gfx_text()` switches color on the fly without incrementing `x` or visible column counters.
   - `lines_add()` tracks the active color code across line-wraps and ignores control bytes when measuring line width against `COLS` (37 cols).
2. **Details Screen (`details()`)**:
   - Header: title on left, port category on right in `C_DIM`.
   - Labels padded to 11 characters (`Installed: `, `Latest:    `, `Location:  `, `ROM:       `, `Files:     `, `Source:    `, `Instructions:`).
   - `Latest` highlighted in amber `CLR_WARN` if unchecked (`not checked (A:check)`) or update available (`%s (update available)`), and green `CLR_GOOD` if up-to-date.
   - Button mapping: `A` performs the primary action (`A:check` when unchecked or up-to-date, `A:update` / `A:install` when available); `Y` also checks; `X` uninstalls.
3. **Download Progress Bar**:
   - ASCII progress bar (`[====>  ]  54%  1.0/1.8 MB`) appears as an inline line in the progress log right under `downloading ...`.
   - Updates in place via `Lines progress` and remains permanently in the scrollable log at 100% after download finishes.
4. **ROM & Built Game Data**:
   - Ports may define `built` in `data` (e.g. Mario Kart 64: `["dc_data/setup.ok", "dc_data/romdata.bin"]`).
   - If a port uncompresses/builds game assets and deletes `rom.z64` on first launch, `check_data()` checks if `built` files exist before reporting `p->rom` as missing.
   - Built folders (e.g. `dc_data/`) are protected as game data in `is_data_file()`.
5. **Lessons (2026-10-19)**:
   - The card is FAT: every file is `rwxrwxrwx`. Never use the exec bit to tell program from data. That once deleted F-Zero X's ROM and saves on "remove, keep data". In `is_data_file()` the catalog's data rules come before any guess about code.
   - Progress output: details go to the log only (`say_log`), and the screen shows one status line (`on_stage` + one bar). Nothing may draw after `gfx_close()`, so call `progress_stop()` first.
   - Releases (from v1.2.0, +0.0.1 each): bump `PM_VERSION` (`src/pm.h`) and `onion/App/PortManager/.pm_version`, write `docs/releases/<tag>.md` in plain language for players, then tag. The release workflow checks the tag and publishes that file as the notes.
   - Every feature has an emulator scenario (`miyoo/emu/scenarios/`). Add or extend one with each change. Setup and assertion directives are listed in `docs/NOTES.md`. The emulator runs only in CI here (no local Python/qemu), so read `contact.png` from the run's artifacts.

## Commit Guidelines
- Commit as: `153522713+ksielick@users.noreply.github.com` (configured in repo config).
- Repo documentation and code in English; chat with owner in Polish.
