# Capture screenshot from Miyoo Mini Plus framebuffer (/dev/fb0)
# Usage: powershell -File tools/screenshot_miyoo.ps1 [out.png] [-Open]
# Without out.png: screenshots/miyoo-<date>-<time>.png in the repo.
param(
    [string]$OutFile = "",
    [string]$MiyooIP = "192.168.0.246",
    [switch]$Open
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$GitSh = "G:\Git\bin\sh.exe"
$AskPass = "/g/Emulacja/redriver2-miyoo/repos/REDRIVER2/miyoo/tools/askpass.sh"
$Ffmpeg = "d:\Program Files\ffmpeg\bin\ffmpeg.exe"

if (-not (Test-Path $Ffmpeg)) {
    $Ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
}

if (-not $OutFile) {
    $ShotDir = Join-Path $RepoRoot "screenshots"
    New-Item -ItemType Directory -Force $ShotDir | Out-Null
    $OutFile = Join-Path $ShotDir ("miyoo-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".png")
}
if (-not [System.IO.Path]::IsPathRooted($OutFile)) {
    $OutFile = Join-Path (Get-Location) $OutFile
}

Write-Host "Capturing framebuffer from Miyoo ($MiyooIP)..." -ForegroundColor Cyan

# 1. Read pan offset and dump /dev/fb0 to SD card
$DumpCmd = "SSH_ASKPASS='$AskPass' SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o NumberOfPasswordPrompts=1 -o LogLevel=ERROR root@$MiyooIP 'cat /sys/class/graphics/fb0/pan; dd if=/dev/fb0 of=/mnt/SDCARD/Roms/mshot.raw bs=640 count=5760 2>/dev/null'"
$PanOut = & $GitSh -c $DumpCmd | Select-Object -First 1
$Y = 0
if ($PanOut -match '^\d+,(\d+)') {
    $Y = $Matches[1]
}

# 2. Convert raw framebuffer using ffmpeg
$RawPath = "\\$MiyooIP\Roms\mshot.raw"
if (-not (Test-Path $RawPath)) {
    Write-Error "Raw framebuffer dump not found at $RawPath"
}

Write-Host "Converting framebuffer to PNG (y offset = $Y)..."
& $Ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s 640x1440 -i $RawPath -vf "crop=640:480:0:$Y,hflip,vflip,format=rgb24" -frames:v 1 $OutFile

# 3. Clean up raw dump on SD card
$CleanCmd = "SSH_ASKPASS='$AskPass' SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o LogLevel=ERROR root@$MiyooIP 'rm -f /mnt/SDCARD/Roms/mshot.raw'"
& $GitSh -c $CleanCmd

Write-Host "Screenshot saved to: $OutFile" -ForegroundColor Green
if ($Open) {
    Invoke-Item $OutFile
}
