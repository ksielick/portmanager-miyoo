# Port Manager for Miyoo Mini Plus (OnionOS)

Install, update and remove native game ports right on your Miyoo Mini Plus, over Wi-Fi.
Each port is downloaded from its author's own GitHub releases.

> **Copyrighted files (ROMs, game data) are never downloaded or hosted.**
> Port Manager installs only the free port itself. For games that need files from the
> original game (a ROM dump, data files), you copy them from your own copy. The port's
> details page tells you exactly which files are needed and where they go.

## Install (first time)

You need a **Miyoo Mini Plus** with **OnionOS** and Wi-Fi.

1. **Turn on Onion's Ports list:** on the Miyoo, open **Apps › Package Manager › Verified**,
   select **Ports Collection** and apply. This adds the *Ports* list to *Games*. You don't
   need to download any of its games.
2. **Download** [PortManager-Miyoo.zip](https://github.com/ksielick/portmanager-miyoo/releases/latest/download/PortManager-Miyoo.zip)
   and unzip it on your computer.
3. **Copy** the `App` and `Roms` folders from the zip to the **root of the SD card**. Let
   them merge with the folders that are already there.
4. Put the card back and turn on the Miyoo. Port Manager is in **Apps › Port Manager** (and
   also in **Games › Ports** as *~Port Manager*).

Later versions install themselves: Port Manager offers its own update when you check for
updates.

## Use

| Button | |
|---|---|
| **X** | all available ports, A-Z (**Select** there: by category) |
| **A** | a game's page: install or update it, and see which file you need to add and where |
| **Y** | check your installed ports (and Port Manager) for updates |
| **Start** | update all (after a check) |
| **Start** on a game's page | play it (when its game files are in place) |
| **Select** on a game's page | all details: where to get the files, checksums, source |
| **Y** on a game's page | restore a backup or install an older version |
| **X** on a game's page | remove it (you can keep your game data and saves) |
| **B** | back |

## Making a game appear in the Ports list

1. Install the port in Port Manager (**X**, choose the game, **A**).
2. Copy the game file shown on its page (with the full path) from your own copy, for example
   `Roms/PORTS/Games/FZeroX/rom.z64`.
3. Open **Games › Ports**: the game is in its category folder (Racing, FPS, RPG, ...).
   If it isn't there yet, run **~Import ports** at the top of the Ports list, or press **Y**
   in the list and choose **Refresh roms**. Onion shows a game only when its files are found.

## Safe by design

- Your saves and the files you added are never overwritten. Before an update, everything it
  replaces is backed up to `Backups/PortManager/`.
- Removing a port can keep your game data and saves.
- Port Manager only writes to and deletes from the Ports folders (`Roms/PORTS/Games`,
  `Shortcuts`, `Imgs`), its backups and its own folder. Anything else on the card (other
  ROMs, saves, BIOS, Onion, other apps) is off limits.
- It checks free space before installing or updating, and stops without changing anything
  if the card is too full.
- **Secure downloads:** everything comes over HTTPS straight from GitHub. The server's
  certificate is checked against the certificate list shipped with the app. Each download is
  then checked against the SHA-256 checksum GitHub gives for it (or against its size where
  GitHub gives no checksum).
- **Keys for private repositories:** to install ports from a private GitHub repository, put
  a GitHub token in `App/PortManager/tokens.txt`, one per line: `owner/repo=<token>` for one
  repository, or `owner/*=<token>` for all of an owner's repositories:
  ```
  someone/their-port=github_pat_...
  someone/*=github_pat_...
  ```
  A token is sent only to GitHub, and only for the repositories it matches. It is never
  written to the log. Use a fine-grained token with read-only access to just those
  repositories. Public ports need no token.

## Ports

Racing: Mario Kart 64 (two ports: cacuracaptors' and mr-finster's), Diddy Kong Racing, F-Zero X ·
FPS: GoldenEye 007, Perfect Dark, Hexen II · Platform: Super Mario 64, Tomb Raider ·
Action: Ocarina of Time, Majora's Mask, Star Fox 64, Super Smash Bros. · RPG: Fallout, Fallout 2,
Paper Mario, Mines of Moria, Monk Tower · Arcade: 3D Pinball Space Cadet, SkiFree, Tennis Vs
Zombies · Puzzle games: Frozen Bubble, Panel Attack · Strategy: Plants vs. Zombies ·
Simulation: Harvest Moon 64.
Coming when its author publishes a release: Zuma Deluxe.

All credit goes to the ports' authors; Port Manager only fetches their releases.

For developers: catalog format and notes in [docs/CATALOG.md](docs/CATALOG.md).
