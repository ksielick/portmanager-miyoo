#!/bin/sh
# "~Port manager" in Onion's Ports list: checks the installed ports, asks before updating,
# shows the result. The work is done by pm.sh (its output: /tmp/pm_check.txt,
# /tmp/pm_update.txt; history: log.txt here).
D=$(cd "$(dirname "$0")" && pwd)
PATH=/mnt/SDCARD/.tmp_update/bin:$PATH
T="Port manager"

busy() {
	rm -f /tmp/dismiss_info_panel
	infoPanel -t "$T" -m "$1" --persistent &
}
unbusy() {
	touch /tmp/dismiss_info_panel
	sync
	sleep 1
}
# lines <file> <grep pattern>: matching lines as one infoPanel/prompt message (\n between)
lines() { grep -E "$2" "$1" | head -12 | cut -d' ' -f3- | sed 's/$/\\n/' | tr -d '\n'; }

busy "Checking for updates..."
sh "$D/pm.sh" check > /tmp/pm_check.txt 2>&1
unbusy

if grep -q '^ERROR' /tmp/pm_check.txt; then
	infoPanel -t "$T" -m "Can't check:\n$(grep '^ERROR' /tmp/pm_check.txt | head -3 | cut -d' ' -f2- | sed 's/$/\\n/' | tr -d '\n')\nIs Wi-Fi on?"
	exit 0
fi
missing=$(lines /tmp/pm_check.txt '^MISSING ')
if ! grep -q '^UPDATE ' /tmp/pm_check.txt; then
	infoPanel -t "$T" -m "Everything is up to date.${missing:+\n\nGame files missing:\n$missing}"
	exit 0
fi

prompt -t "$T" -m "Updates:\n$(lines /tmp/pm_check.txt '^UPDATE ')" "Update" "Cancel"
[ $? -eq 0 ] || exit 0

busy "Updating..."
sh "$D/pm.sh" update > /tmp/pm_update.txt 2>&1
unbusy
done=$(grep -E ' -> .*replaced|^ERROR' /tmp/pm_update.txt | head -12 | sed 's/$/\\n/' | tr -d '\n')
infoPanel -t "$T" -m "${done:-Nothing was updated.}"
