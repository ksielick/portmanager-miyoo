# Deploy PortManager to Miyoo Mini Plus (OnionOS) over Wi-Fi / SSH
# Usage: powershell -File tools/deploy_to_miyoo.ps1
param(
    [string]$MiyooIP = "192.168.0.246"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$GitSh = "G:\Git\bin\sh.exe"
$AskPass = "/g/Emulacja/redriver2-miyoo/repos/REDRIVER2/miyoo/tools/askpass.sh"

Write-Host "=== Deploying PortManager to Miyoo ($MiyooIP) ===" -ForegroundColor Cyan

# 1. Check network connectivity
Write-Host "[1/5] Checking connectivity to $MiyooIP..."
if (-not (Test-Connection -ComputerName $MiyooIP -Count 1 -Quiet)) {
    Write-Error "Miyoo is not reachable at $MiyooIP! Make sure Wi-Fi is enabled on the device."
}

# 2. Backup current state on PC
$DateTag = Get-Date -Format "yyyyMMdd-HHmmss"
$BackupDir = Join-Path $RepoRoot "backup\$DateTag-before-deploy"
New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
$BackupTar = Join-Path $BackupDir "portmanager-backup.tar"

Write-Host "[2/5] Creating PC backup: $BackupTar..."
$BackupCmd = "SSH_ASKPASS='$AskPass' SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o NumberOfPasswordPrompts=1 -o LogLevel=ERROR root@$MiyooIP 'tar -C /mnt/SDCARD -cf - App/PortManager' > '$($BackupTar.Replace('\', '/'))'"
& $GitSh -c $BackupCmd
if ((Test-Path $BackupTar) -and (Get-Item $BackupTar).Length -gt 1024) {
    Write-Host "      Backup verified ($((Get-Item $BackupTar).Length) bytes)." -ForegroundColor Green
} else {
    Write-Warning "Remote App/PortManager might be empty or backup had warnings."
}

# 3. Copy Roms shortcuts and images over SMB (or SSH if SMB fails)
Write-Host "[3/5] Updating Roms shortcuts and images..."
$SmbRoms = "\\$MiyooIP\Roms"
if (Test-Path $SmbRoms) {
    Remove-Item -Force -ErrorAction SilentlyContinue "$SmbRoms\PORTS\Shortcuts\~Port manager.miyoocmd"
    Remove-Item -Force -ErrorAction SilentlyContinue "$SmbRoms\PORTS\Imgs\~Port manager.png"
    Copy-Item -Force "$RepoRoot\onion\Roms\PORTS\Shortcuts\~Port Manager.miyoocmd" "$SmbRoms\PORTS\Shortcuts\~Port Manager.miyoocmd"
    Copy-Item -Force "$RepoRoot\onion\Roms\PORTS\Imgs\~Port Manager.png" "$SmbRoms\PORTS\Imgs\~Port Manager.png"
    Write-Host "      Copied to SMB share: Shortcuts and Imgs." -ForegroundColor Green
} else {
    Write-Host "      SMB share not accessible, falling back to SSH..."
    $RemoteSc = "/mnt/SDCARD/Roms/PORTS/Shortcuts"
    $RemoteImgs = "/mnt/SDCARD/Roms/PORTS/Imgs"
    $RepoUnix = $RepoRoot.Replace('\', '/')
    $ScCmd = "SSH_ASKPASS='$AskPass' SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o LogLevel=ERROR root@$MiyooIP `"rm -f '$RemoteSc/~Port manager.miyoocmd' && cat > '$RemoteSc/~Port Manager.miyoocmd'`" < '$RepoUnix/onion/Roms/PORTS/Shortcuts/~Port Manager.miyoocmd'"
    & $GitSh -c $ScCmd
    $ImgCmd = "SSH_ASKPASS='$AskPass' SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o LogLevel=ERROR root@$MiyooIP `"rm -f '$RemoteImgs/~Port manager.png' && cat > '$RemoteImgs/~Port Manager.png'`" < '$RepoUnix/onion/Roms/PORTS/Imgs/~Port Manager.png'"
    & $GitSh -c $ImgCmd
}

# 4. Stream App/PortManager tar over SSH
Write-Host "[4/5] Deploying App/PortManager files over SSH..."
$AppDir = "$($RepoRoot.Replace('\', '/'))/onion/App/PortManager"
$DeployCmd = "cd '$AppDir' && tar -cf - . | SSH_ASKPASS='$AskPass' SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o NumberOfPasswordPrompts=1 -o LogLevel=ERROR root@$MiyooIP 'mkdir -p /mnt/SDCARD/App/PortManager && tar -xf - -C /mnt/SDCARD/App/PortManager && chmod +x /mnt/SDCARD/App/PortManager/portmanager /mnt/SDCARD/App/PortManager/launch.sh && rm -f /mnt/SDCARD/App/PortManager/portmaster'"
& $GitSh -c $DeployCmd

# 5. Clear OnionOS list cache
Write-Host "[5/5] Clearing OnionOS list cache..."
$CacheCmd = "SSH_ASKPASS='$AskPass' SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o LogLevel=ERROR root@$MiyooIP 'rm -f /mnt/SDCARD/Roms/PORTS/Shortcuts/*cache*.db'"
& $GitSh -c $CacheCmd

Write-Host "=== Deployment finished successfully! ===" -ForegroundColor Green
