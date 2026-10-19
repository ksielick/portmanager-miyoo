param(
    [string]$MiyooIP = "192.168.0.246"
)

$GitSh = "G:\Git\bin\sh.exe"
$AskPass = "/g/Emulacja/redriver2-miyoo/repos/REDRIVER2/miyoo/tools/askpass.sh"

$remoteScript = @'
echo "=== EXISTING BACKUPS IN Backups/PortManager ==="
for f in /mnt/SDCARD/Backups/PortManager/*; do
    if [ -d "$f" ]; then
        sz=$(du -sk "$f" 2>/dev/null | awk '{print $1}')
        mb=$(awk "BEGIN {printf \"%.2f\", $sz/1024}")
        echo "DIR  ${mb} MB  $(basename "$f")"
    elif [ -f "$f" ]; then
        sz=$(ls -sk "$f" 2>/dev/null | awk '{print $1}')
        mb=$(awk "BEGIN {printf \"%.2f\", $sz/1024}")
        echo "FILE ${mb} MB  $(basename "$f")"
    fi
done

echo ""
echo "=== CURRENT INSTALLED PORTS ESTIMATED BACKUP SIZES ==="
# Check ports defined in catalog.json
for p in DiddyKongRacing GoldenEye Hexen2 MarioKart64 OcarinaOfTime OpenLara PaperMario PerfectDark StarFox64 SuperMario64 SuperSmashBros Fallout "Fallout 2" "Plants vs. Zombies" "Zuma Deluxe"; do
    d="/mnt/SDCARD/Roms/PORTS/Games/$p"
    if [ -d "$d" ]; then
        # Total size of binaries/executables and scripts in root
        binsz=0
        for b in "$d"/*; do
            if [ -f "$b" ]; then
                # check if executable or script or .ini/.cfg
                case "$b" in
                    *.sh|*.ini|*.cfg|*.txt|*.port|*.so*)
                        s=$(ls -sk "$b" 2>/dev/null | awk '{print $1}')
                        binsz=$((binsz + s))
                        ;;
                    *)
                        if [ -x "$b" ]; then
                            s=$(ls -sk "$b" 2>/dev/null | awk '{print $1}')
                            binsz=$((binsz + s))
                        fi
                        ;;
                esac
            fi
        done

        # Check backup folders if any
        bk_extra=0
        for sub in data userdata DATA/SAVEGAME SAVEGAME saves; do
            if [ -d "$d/$sub" ]; then
                s=$(du -sk "$d/$sub" 2>/dev/null | awk '{print $1}')
                bk_extra=$((bk_extra + s))
                echo "     [$p] subfolder $sub: $(awk "BEGIN {printf \"%.2f\", $s/1024}") MB"
            fi
        done

        total_k=$((binsz + bk_extra))
        tot_mb=$(awk "BEGIN {printf \"%.2f\", $total_k/1024}")
        bin_mb=$(awk "BEGIN {printf \"%.2f\", $binsz/1024}")
        echo "PORT $p: Total ~$tot_mb MB (Binaries/scripts: $bin_mb MB)"
    fi
done
'@

$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remoteScript))
$cmd = "SSH_ASKPASS='$AskPass' SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o NumberOfPasswordPrompts=1 -o LogLevel=ERROR root@$MiyooIP 'echo $b64 | base64 -d | sh'"
& $GitSh -c $cmd
