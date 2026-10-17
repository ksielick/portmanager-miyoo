Fallout (1997) - Miyoo Mini Plus / OnionOS Port
==================================================

An unofficial port of fallout1-ce (https://github.com/alexbatalov/fallout1-ce), the community
re-implementation of the original Fallout, to run as a standalone OnionOS Port on the Miyoo Mini
Plus - a handheld with no 3D graphics acceleration.

YOU NEED YOUR OWN GAME FILES
-----------------------------
This port does NOT include and will NEVER include the Fallout data files (MASTER.DAT,
CRITTER.DAT, the data folder) - they are the property of Interplay/Bethesda. You need a
legitimate copy of the game (GOG or Steam) and must copy those files yourself.

FEATURES
--------
- Software rendering (the Miyoo Mini Plus has no 3D GPU)
- A full control scheme adapted for the Miyoo Mini Plus' hardware, which has no analog sticks
- The D-pad acts as a mouse cursor
- A custom on-device text entry system (D-pad + buttons) for naming your character, save games,
  etc., since the device has no physical keyboard
- An in-game "HELP" button in the Options menu, showing a Miyoo Mini control reference screen
- Working audio and video, including cutscenes

INSTALLATION
------------
1. Extract this zip's contents to the root of your OnionOS SD card.
2. Copy the following files from your legitimate Fallout installation (GOG/Steam) into
   Roms/PORTS/Games/Fallout/:
     - MASTER.DAT
     - CRITTER.DAT
     - the data/ folder
3. On the device, open the "Ports" menu - "Fallout" should appear in the list. If not, use
   "refresh roms" at the bottom of the list.

   IMPORTANT: don't delete the "miyoo_patches" folder that ships alongside the binary - it
   holds two small art overrides (an enlarged options menu background and the Miyoo-specific
   Quick Guide help screen) that this port needs. You don't need to do anything with it, just
   leave it where it is.

   IMPORTANT: do not copy fallout.cfg from your PC installation. If you
   already did, delete it: the game creates its own on first launch. A PC
   fallout.cfg overrides this port's settings and breaks the in-game HELP
   screen.

CONFIGURING QUALITY-OF-LIFE SETTINGS
-------------------------------------
Most settings live in fallout.cfg, generated automatically the first time you run the game. Open
it in a text editor and change values there if you'd like to adjust anything (screen options,
etc.) - the same way you would on PC.

CONTROLS
--------
  D-pad          Move mouse cursor
  L1             Right click
  R1             Left click
  Select (hold)  Modifier - hold and press another button for its second function (see below)
  Start          Enter / confirm
  Menu Key       Esc / Exit (fires on release, so Menu+Power for a screenshot won't accidentally
                 exit the game)

  Button       Without Select              With Select held
  ------------------------------------------------------------
  D-pad        Moves the mouse cursor      Scrolls the camera/map
  A            Attack                      Skilldex
  B            End Turn                    Character screen
  X            Slow mouse (hold)           Inventory
  Y            End Combat                  Pip-Boy
  L1           Right click                 Quickload (F7)
  R1           Left click                  Quicksave (F6)
  L2           Switch active item          Map (Automap)
  R2           Switch active item's mode   Center screen on player
  Start        Enter / confirm             -
  Select       (modifier)                  -
  Menu Key     Esc / Menu/Return/Exit      -

Quicksave and Quickload have a short cooldown after firing (to avoid the underlying hardware's
key-repeat behavior from spamming save/load repeatedly).

TYPING TEXT (character name, save names, etc.)
------------------------------------------------
Since the device has no keyboard, text entry works by cycling through letters directly in the
game's own text field:

  D-pad Up/Down    cycles through the alphabet/numbers/space at the current position
  D-pad Left       toggles uppercase/lowercase for the current letter
  D-pad Right      inserts a space directly and moves to the next position
  A                confirms the current letter and moves to the next position
  B                deletes the last confirmed letter
  Start            confirms the whole text entry (Enter)
  Menu Key         cancels the whole text entry (Esc)

An in-game "Quick Guide" screen with this control scheme is available from the Options menu at
any time.

KNOWN ISSUES
------------
- Intro/cutscene videos don't stretch to fill the screen (they render at their original
  resolution, centered) - the same behavior is present in the Portmaster version of this port,
  so it appears to be an inherited limitation of the original engine.

CHANGELOG
---------
- v1.2.0 - Save on power off: turning the device off with the power
  button while playing now saves your progress in a separate, hidden
  slot (it never replaces your own saves), and the next time you turn
  the device on the game starts by itself and picks up right where you
  left off, skipping the intro and the main menu. It saves on the map
  and on your turn in combat: open windows (inventory, Pip-Boy, menus)
  are closed first, and enemy turns and scripted scenes are waited for.
  In a conversation or on the world map the game closes without saving,
  as before.
- v1.1.5 - The D-pad mouse cursor now moves at the same speed on every
  screen: it is no longer slower on screens with a text field (character
  creation, save names, etc.) or in busy maps. Lower CPU use, and so
  better battery life, in dialogs, the Pip-Boy, the world map, the
  save/load screens and the character screen: the game no longer keeps a
  CPU core busy while it waits between frames.
- v1.1.4 - Fixed the occasional crash when skipping the intro videos
  quickly (the sound engine's thread locks were not working on this
  device). Lower CPU use, and so better battery life, on menus, dialogs,
  the inventory, the world map and most maps: the screen is only redrawn
  when something changes, color cycling (water, fire, monitors) no
  longer forces full-screen redraws when none of those colors are on
  screen, and audio mixing is about 10x lighter. Map and save loading is
  also much faster.
- v1.1.3 - Fixed buttons (notably Start and Select) sometimes ignoring
  presses, especially in long sessions. Crash reports (crash_log.txt) now
  show exactly where a crash happened.
- v1.1.2 - Updated the in-game Quick Guide help screen with the on-device
  text entry controls. Updated the installation instructions: fallout.cfg
  should no longer be copied from a PC installation, since it overrides
  this port's settings and breaks the HELP screen. Added a crash logger:
  if the game crashes, it writes a crash_log.txt file to the game folder,
  which helps a lot when reporting the problem.
- v1.1.1 - Added an in-game "HELP" button to the Options menu, showing a Miyoo Mini control
  reference screen. Fixed the Menu key so its Esc action fires on release instead of press,
  making the OnionOS Menu+Power screenshot combo safe to use without exiting the game.
- v1.1.0 - Fixed an intermittent crash caused by an audio buffer over-read. Fixed a persistent,
  constant audio latency by requesting 44.1 kHz audio output instead of 22050 Hz.
- v1.0.0 - Initial release.

BUILDING FROM SOURCE
---------------------
Full source code and build instructions are available in this port's GitHub repository:
https://github.com/cacuracaptors/fallout1-ce-miyoomini

CREDITS
-------
- Alexander Batalov - fallout1-ce - https://github.com/alexbatalov/fallout1-ce
- steward-fu - SDL2 for the Miyoo Mini (Plus)
- shauninman - union-miyoomini-toolchain
- Interplay Entertainment / Black Isle Studios - the original Fallout (1997)
