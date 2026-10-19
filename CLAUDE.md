# portmanager-miyoo

Port manager for the Miyoo Mini Plus (OnionOS), tests (public repo ksielick/portmanager-miyoo since 2026-10-17; commit as 153522713+ksielick@users.noreply.github.com, already set in the repo config).
Read `docs/NOTES.md` (decisions, findings, the card's state) and `docs/PLAN.md` first; keep
NOTES updated after each step. Catalog: `catalog.json`, format in `docs/CATALOG.md`.

- The owner's rules: Onion's Ports list only (never `App/`), thematic Shortcuts category
  folders, copyrighted files only through player instructions, a PC backup before changing
  the card.
- Device access and automated tools:
  - Deploy to device: `powershell -ExecutionPolicy Bypass -File tools/deploy_to_miyoo.ps1` (backs up `App/PortManager` to `backup/`, copies shortcuts/imgs to `\\192.168.0.246\Roms`, streams `App/PortManager` tar over SSH with chmod +x, removes old `portmaster`, clears `Shortcuts_cache*.db`).
  - Capture screenshot from device: `tools/screenshot_miyoo.bat` (double-click; saves to `screenshots/` and opens it) or `powershell -ExecutionPolicy Bypass -File tools/screenshot_miyoo.ps1 [out.png] [-Open]` (dumps `/dev/fb0`, converts with ffmpeg using pan offset).
  - Low-level tools (`mssh`/`mscp`, SMB share `\\192.168.0.246\Roms`) live in `G:\Emulacja\redriver2-miyoo\repos\REDRIVER2\miyoo\tools\`.
- Repo files in English; chat with the owner in Polish.
