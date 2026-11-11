# Changelog

All notable changes to Port Manager are documented here. Releases are published on [GitHub Releases](https://github.com/ksielick/portmanager-miyoo/releases).

---

## [v1.2.11] - 2026-11-11

### Fixed
- **Downloads from GitHub stopping at 0%**: Port Manager now downloads everything using Onion's `curl` instead of `wget`. Onion's built-in `wget` (uclient-fetch on mbed TLS from 2015) started timing out on github.com release downloads in November 2026.
- **Failures now report detailed reason**: When a download fails, the exact error reason from the downloader (such as "Connection timed out" or "Could not resolve host") is shown on screen and recorded in `App/PortManager/log.txt`.
- **Large downloads on slow Wi-Fi**: Stalls are detected after 60 seconds without data (`--speed-limit 1 --speed-time 60`) instead of cutting off total transfer after a fixed duration.

---

## [v1.2.10] - 2026-11-06

### Added
- **Quake (TyrQuake)** and **Quake II** added to catalog (FPS). Requires original game data (Steam/GOG).
- **Banjo-Kazooie**, **Pilotwings 64**, and **The New Tetris** added from mr-finster's miyoo64 (requires USA ROM cartridge dumps).
- **Zelda - Ocarina of Time** by cacuracaptors added (Action, separate port alongside mr-finster's, supports Rev A 1.1 ROM).

---

## [v1.2.9] - 2026-10-23

### Added
- **Backups management screen (`Select`)**: Inspect all port backups in `Backups/PortManager/`, view total space taken, compact them (strips logs and rebuilt data, keeps all saves), or delete unused backups.
- **Backups named by the version they hold**: Restores now display the exact version contained in the backup archive.
- **Marquee scrolling**: Long port names scroll when selected in the list.

### Changed
- New backups of N64 ports leave out reconstructed packs and logs, significantly reducing backup sizes.
- Single save files (e.g. `eeprom.bin`) are now included in backups alongside save folders.
- `Select` on the main list opens backups; self-update is integrated under `Y` (Check).

---

## [v1.2.8] - 2026-10-22

### Added
- **Onion's Ports Collection integration**: Added C-Dogs, OpenTyrian, Cave Story, Wizznic, Dinothawr, and Hocoslamfy.
- Preserves user high scores, configurations, and saves when updating over existing copies.
- Accurate file count in extraction progress bar and new "Checking files" verification stage.
- Informative check failure messages (invalid token, repo not found, rate limit exhausted).

---

## [v1.2.7] - 2026-10-22

### Added
- **Quick-launch from details (`Start`)**: Pressing `Start` on an installed game's page immediately launches the game through OnionOS and updates Onion's recently played list.

---

## [v1.2.6] - 2026-10-22

### Added
- Press `Start` on a game's page to launch it directly.

---

## [v1.2.5] - 2026-10-22

### Fixed
- "Remove, keep data" now protects cutscene videos (`FMV/`, `Movies/`, `.RPL`, `.STR`) and custom data across all folder depths (e.g. Tomb Raider cutscenes).
- Detailed log output of every file deleted during port removal.

---

## [v1.2.4] - 2026-10-20

### Added
- **Mario Kart 64 (mr-finster)** added to catalog alongside cacuracaptors' port (separate folder, shortcut, and saves).

---

## [v1.2.3] - 2026-10-20

### Added
- **Majora's Mask** and **Harvest Moon 64** added to catalog from miyoo64.
- Step-by-step update check under `Y` (Port Manager first, then catalog, then ports).
- Self-update restarts Port Manager before continuing port checks.
- Automatic catalog download if missing on the SD card.

---

## [v1.2.2] - 2026-10-19

### Added
- **Frozen Bubble**, **Panel Attack**, **Tennis Vs Zombies**, **Mines of Moria**, and **Monk Tower** added to catalog.
- Fast direct-to-destination unpacking without double card writes.
- Full A-Z sorting under `X` with `Select` category toggle.
- Port development status indicator (`prerelease`, `playable`) in details.

---

## [v1.2.1] - 2026-10-19

### Added
- Free space verification before download/extraction.
- Strict safety boundary: only writes to designated Ports folders and backups.
- Normalized version comparison preventing redundant updates (e.g. `1.0.1` vs `v1.0.1`).

---

## [v1.2.0] - 2026-10-19

### Initial Release
- Native Wi-Fi port manager for Miyoo Mini Plus (OnionOS).
- Automatic backup before update, clean uninstaller with "keep data" mode, progress reporting, and catalog integration.
