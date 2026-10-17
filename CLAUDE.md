# portmanager-miyoo

Port manager for the Miyoo Mini Plus (OnionOS), tests (public repo ksielick/portmanager-miyoo since 2026-10-17; commit as 153522713+ksielick@users.noreply.github.com, already set in the repo config).
Read `docs/NOTES.md` (decisions, findings, the card's state) and `docs/PLAN.md` first; keep
NOTES updated after each step. Catalog: `catalog.json`, format in `README.md`.

- The owner's rules: Onion's Ports list only (never `App/`), thematic Shortcuts category
  folders, copyrighted files only through player instructions, a PC backup before changing
  the card.
- Device access and its tools (`mssh`/`mscp`, SMB share `\\192.168.0.246\Roms`) live in
  `G:\Emulacja\redriver2-miyoo\repos\REDRIVER2\miyoo\tools\`, together with today's miyoo64
  updater (`update_miyoo64.sh`, `miyoo64_apply.sh`) this project grows out of.
- Repo files in English; chat with the owner in Polish.
