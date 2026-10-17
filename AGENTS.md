# AGENTS.md - Instructions for AI Agents in portmanager-miyoo

## Deployment to Miyoo Mini Plus
To deploy the latest version of Port Manager to the physical Miyoo device over LAN/Wi-Fi:
```powershell
powershell -ExecutionPolicy Bypass -File tools/deploy_to_miyoo.ps1
```
This automated script:
1. Verifies connectivity to the Miyoo Mini Plus at `192.168.0.246`.
2. Creates an automatic verified PC backup in `backup/<timestamp>-before-deploy/portmanager-backup.tar`.
3. Copies `~Port manager.miyoocmd` and `~Port manager.png` to `\\192.168.0.246\Roms` (or SSH fallback).
4. Streams `onion/App/PortManager/` tar over SSH into `/mnt/SDCARD/App/PortManager/`, sets `chmod +x` on binaries and scripts, and deletes any old `portmaster` binary.
5. Clears OnionOS list cache (`/mnt/SDCARD/Roms/PORTS/Shortcuts/*cache*.db`).

## Capturing Screenshot from Miyoo Mini Plus
To capture a real-time framebuffer screenshot from the device:
```powershell
powershell -ExecutionPolicy Bypass -File tools/screenshot_miyoo.ps1 [out.png]
```
This dumps `/dev/fb0`, queries the pan offset, and converts to PNG using `ffmpeg`.

## Commit Guidelines
- Commit as: `153522713+ksielick@users.noreply.github.com` (configured in repo config).
- Repo documentation and code in English; chat with owner in Polish.
