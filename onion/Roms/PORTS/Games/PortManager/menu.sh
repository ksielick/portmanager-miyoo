#!/bin/sh
# "~Port manager" in Onion's Ports list. Opens with the installed ports, from the catalog on
# the card and without the network; "Check for updates" (installed ports only) and "All ports
# in the catalog" are chosen from that list. pm.sh does the work, this shows it with Onion's
# prompt (a list, returns the chosen index, 255 for B) and infoPanel (255 characters a panel).
D=$(cd "$(dirname "$0")" && pwd)
PATH=/mnt/SDCARD/.tmp_update/bin:$PATH
T="Port manager"
TAB=$(printf '\t')

busy() {
	rm -f /tmp/dismiss_info_panel
	infoPanel -t "$T" -m "$1" --persistent &
}
unbusy() {
	touch /tmp/dismiss_info_panel
	sync
	sleep 1
}

# show <title> <file>: the text in panels of up to ~230 characters (A: next, B: stop)
show() {
	awk -v max=230 '
		function out() { if (p != "") print p; p = "" }
		{
			n = split($0, w, " ")
			for (i = 1; i <= n; i++) {
				if (length(p) + length(w[i]) + 1 > max) out()
				p = (p == "" || p ~ /\\n$/) ? p w[i] : p " " w[i]
			}
			if (p != "") p = p "\\n"
		}
		END { out() }' "$2" > /tmp/pm_pages.txt
	n=$(wc -l < /tmp/pm_pages.txt)
	i=0
	while IFS= read -r page; do
		i=$((i + 1))
		t=$1
		[ "$n" -gt 1 ] && t="$1 ($i/$n)"
		infoPanel -t "$t" -m "$page" < /dev/null || break
	done < /tmp/pm_pages.txt
}

port_info() {
	sh "$D/pm.sh" info "$1" > /tmp/pm_info.txt 2>&1
	show "$T" /tmp/pm_info.txt
}

check_updates() {
	busy "Checking for updates..."
	sh "$D/pm.sh" check > /tmp/pm_check.txt 2>&1
	unbusy
	if grep -q '^ERROR' /tmp/pm_check.txt; then
		{ echo "Can't check:"; grep '^ERROR' /tmp/pm_check.txt | head -3 | cut -d' ' -f3-; echo "Is Wi-Fi on?"; } > /tmp/pm_msg.txt
		show "$T" /tmp/pm_msg.txt
		return
	fi
	grep '^UPDATE ' /tmp/pm_check.txt > /tmp/pm_updates.txt
	if [ ! -s /tmp/pm_updates.txt ]; then
		echo "All installed ports are up to date." > /tmp/pm_msg.txt
		show "$T" /tmp/pm_msg.txt
		return
	fi
	set -- "Update all"
	while IFS= read -r l; do set -- "$@" "$(echo "$l" | cut -d' ' -f3-)"; done < /tmp/pm_updates.txt
	prompt -t "$T: updates" "$@"
	r=$?
	[ $r -eq 255 ] && return
	if [ $r -eq 0 ]; then
		ids=$(cut -d' ' -f2 /tmp/pm_updates.txt)
	else
		ids=$(sed -n "${r}p" /tmp/pm_updates.txt | cut -d' ' -f2)
	fi
	busy "Updating..."
	sh "$D/pm.sh" update $ids > /tmp/pm_update.txt 2>&1
	unbusy
	grep -E ' -> .*replaced|^ERROR|kept your|^backup' /tmp/pm_update.txt > /tmp/pm_msg.txt
	[ -s /tmp/pm_msg.txt ] || echo "Nothing was updated." > /tmp/pm_msg.txt
	show "$T" /tmp/pm_msg.txt
}

all_ports() {
	sh "$D/pm.sh" all > /tmp/pm_all.txt 2>&1
	asel=0
	while :; do
		set --
		while IFS="$TAB" read -r id label; do set -- "$@" "$label"; done < /tmp/pm_all.txt
		prompt -t "$T: all ports" -s "$asel" "$@"
		r=$?
		[ $r -eq 255 ] && return
		asel=$r
		port_info "$(sed -n "$((r + 1))p" /tmp/pm_all.txt | cut -f1)"
	done
}

sel=0
while :; do
	sh "$D/pm.sh" list > /tmp/pm_list.txt 2>&1
	set -- "Check for updates" "All ports in the catalog"
	while IFS="$TAB" read -r id label; do set -- "$@" "$label"; done < /tmp/pm_list.txt
	prompt -t "$T" -s "$sel" "$@"
	r=$?
	[ $r -eq 255 ] && exit 0
	sel=$r
	case $r in
	0) check_updates ;;
	1) all_ports ;;
	*) port_info "$(sed -n "$((r - 1))p" /tmp/pm_list.txt | cut -f1)" ;;
	esac
done
