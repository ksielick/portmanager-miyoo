# Plan: a port manager for the Miyoo (like PortMaster, but per-author)

Idea from the owner, 2026-10-17 (moved here from redriver2-miyoo's `miyoo/PORT_MANAGER.md`).
An app on the Miyoo that lists, installs and updates native ports, downloading each one
straight from its author's GitHub releases instead of from one central repo of zips. Today
only the miyoo64 games are handled, from the PC, by redriver2-miyoo's
`miyoo/tools/update_miyoo64.sh` + `miyoo64_apply.sh`; this grows that into a general tool.
Findings, decisions and the card's state: `NOTES.md`. Catalog format: `CATALOG.md`.

## What PortMaster-New does (checked 2026-10-17)

github.com/PortsMaster/PortMaster-New (MIT), client github.com/PortsMaster/PortMaster-GUI (MIT,
Python, "harbourmaster").

What they changed vs the old PortMaster repo (a git repo full of zips, impossible to diff):
- Ports are stored **unzipped** in git: `ports/<portname>/` with `port.json`, `README.md`,
  `screenshot.jpg` (640x480), `gameinfo.xml`, optional `cover.jpg`, `Port Name.sh` and the
  port folder `<portname>/` (with `licenses/LICENSE.<component>.txt`). Files over 90 MB are
  split into 50 MB chunks (`tools/build_data.py`).
- `tools/build_release.py` checks a port (`--do-check`, run on every PR: required files, name
  clashes across all ports, gameinfo.xml in sync) and zips **only the ports that changed**.
- A release workflow (manual, self-hosted runner) publishes a dated release
  (`2026-10-15_2356`) holding only the changed zips (the latest one: 9 zips) plus the index.
  Each port's entry points at the release its zip was last built in, so old zips stay where
  they are and nothing is re-uploaded.
- Index files in the latest release:
  - `ports.json` (2.5 MB, 1441 ports): per port `name` (zip), `items` (top-level files/dirs
    it installs), `attr` (title, desc, inst, porter, genres, image, `rtr` = ready to run,
    `runtime`, `store`, `availability` free/paid, `reqs`, `arch`, `min_glibc`) and `source`
    (`url`, `md5`, `size`, `date_added`, `date_updated`, `release_id`). Plus `utils`:
    `images.NNN.zip` chunks (screenshots), `gameinfo.zip`.
  - `manifest.json` (14 MB): md5 of every file of every port (lets a client tell files the
    user changed from stale ones).
  - `ports_status.json`, `runtimes_zips.json`: shared runtimes (dotnet, Godot frt, AGS, ...)
    as squashfs images the port's script mounts; the client only downloads them.
- `AGENTS.md`: they reject AI-written, verbose submissions without discussion. If we ever
  submit something there, follow it to the letter.

The client already supports several sources: each is a JSON file
(`020_portmaster.source.json`: `api`, `name`, `url`, `prefix`) with an API type —
`PortMasterV3` (a `ports.json` URL; defaults: PortMaster and PortMaster Multiverse),
`GitHubRawReleaseV1`, `GitHubRepoV1` (scans a repo tree). Every source is still a curated
index, though; none of them reads an author's own releases.

Why not just run PortMaster here: it targets aarch64/armhf handhelds with a GPU (ArkOS,
Rocknix, muOS, ...); arch counts in the index: aarch64 990, armhf 242, none 247, x86_64 93.
Onion has no Python (PortMaster-GUI needs it), the launch scripts assume PortMaster's control
folder/gptokeyb, and the Mini Plus has no 3D GPU, so most ports wouldn't run anyway. The
native Miyoo ports (miyoo64, mk64-miyoomini, ours) aren't in PortMaster at all.

Borrow from PortMaster: the per-port fields (title, desc, inst, porter, genres, `rtr`,
`reqs`, store/availability), md5/size/date per entry, a file manifest for clean updates,
screenshots in chunked zips, a CI check for catalog entries (does the repo exist, does the
asset match). Don't borrow: a central repo of zips, the gptokeyb/control.txt launch stack,
Python.

## Design

- **Catalog, not hosting** (`catalog.json`, format in `CATALOG.md`): where each port is
  released and how it installs; no zip is ever copied. Descriptions (name, notes, image,
  porter, status) come from [MiyooMini-Ports](https://github.com/Producdevity/MiyooMini-Ports),
  an existing catalog of the same kind (links to authors' `/releases`), matched by repo.
- The client asks each author's repo once: `GET /repos/{owner}/{repo}/releases/latest`
  (miyoo64 = one request for 10 games). Tag vs the installed version file = update or not.
  The asset (picked by the entry's regex) is downloaded from the author's release and checked
  before anything is unpacked: GitHub's `digest` (sha256) when there is one (miyoo64 has it,
  cacuracaptors' releases don't), else a `.sha256` file in the release, else size only.
- Unauthenticated API limit: 60 requests/hour per IP. One request per repo plus ETag
  (`If-None-Match`; GitHub says a 304 doesn't count against the limit — check that
  unauthenticated) is plenty. Optional token in the config (needed for private repos).
- Authors can opt in with their own `miyoo-port.json` asset in the release; when present it
  overrides the catalog entry, so the catalog shrinks to a list of repos.
- **Onion's Ports list only**: games in `Roms/PORTS/Games/<dir>/`, shortcuts in
  `Roms/PORTS/Shortcuts/<Category>/`, pictures in `Roms/PORTS/Imgs/`; nothing in `App/`
  (layout `app-to-ports` for zips that install there, like miyoo64's).
- **Copyrighted files (ROMs, game data) are never downloaded, hosted or linked to unofficial
  copies: the player adds them.** Every entry that needs them carries instructions (`get`),
  shown by the app when the files are missing:
  - where to get them legally (store links, like PortMaster's `store` + `availability`; a
    free demo if there is one, e.g. Carmageddon's);
  - which version is needed (region/revision, md5 — the app checks it and says what is wrong);
  - where to put them on the card (exact folder and file name);
  - a PC step when one is needed (e.g. dethrace's `install_gog.bat` unpacking the GOG
    installer), with what to copy afterwards and how (card reader for thousands of files).
- Data check: the entry's `data` list (Onion's own `GameDataFile` check isn't enough: many
  ports name their binary there). The shortcut we write should still name a real data file
  in `GameDataFile`, so Onion's "Import ports" hides a game whose data is missing.
- Install/update = `miyoo64_apply.sh` generalised: only files from the zip are written;
  anything else in the game folder (the player's data, saves, settings) is never touched;
  before the first replaced file the game's saves and every replaced file go to
  `Backups/<id>/<date>-before-<tag>/`; shortcuts/pictures the user changed are kept (md5 of
  what we installed, like PortMaster's `manifest.json`); the version is written last.
- ROMs: the player puts a ROM where the entry says (or anywhere under a set folder); the
  client checks it by md5 (z64/v64/n64 byte orders converted), or says which dump is needed.
- Uninstall removes only files from our own manifest; saves go to Backups first.
- After changes: Onion's list cache is reset (the `Shortcuts_cache*.db` file; Onion's own
  `reset_list.sh` has a side effect, see `NOTES.md`) and Onion's recent list is pointed at
  moved shortcuts.

## Steps

1. PC: turn `update_miyoo64.sh` into a catalog-driven `miyoo_ports.sh` here (miyoo64 games,
   cacuracaptors' ports, REDRIVER2, Carmageddon, Carmageddon Splat Pack) — same backups and
   checks, more layouts and asset formats (zip first; 7z/tar later).
   REDRIVER2 and dethrace-miyoo have no GitHub releases yet (they're installed from their
   build folders): give them release workflows (binary + scripts zip, no game data) first.
2. Miyoo: the same logic as a shell script (wget + jq + unzip) started from the Ports list
   (`~Port manager.miyoocmd`, like Onion's `~Import ports`), so even the manager isn't in
   Apps; first version shows a text report (check / update all).
3. Native UI (the SDL subset we already have, 640x480): list with picture, installed/latest
   version, install/update/remove, data/ROM status with the `get` instructions.
4. Later: the rest of MiyooMini-Ports (~55 entries with releases), maybe a PortMaster source
   for the few armhf ports that work without a GPU.

Open questions:
- where the catalog lives: this repo (public since 2026-10-17; the device fetches
  `catalog.json` from raw.githubusercontent.com), or
  contributing install data to MiyooMini-Ports instead;
- our own ports' repos are private (redriver2-miyoo, dethrace-miyoo): their releases need a
  token in the client's config until they go public;
- licences of what the catalog points to (we only link, the player downloads from the author);
- Wi-Fi is off by default on Onion: the app has to say so instead of hanging.
