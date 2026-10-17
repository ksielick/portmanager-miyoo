#!/bin/sh
# The port manager's menu, made of Onion's own Ports list: the folder
# Roms/PORTS/Shortcuts/~Port manager holds one .miyoocmd per installed port (its details and
# game-file instructions), "~Check for updates", after a check "~Update all" and
# "~Update <port> <old> to <new>", "~Refresh list", and the folder "~All ports in the catalog".
# Each entry runs this script; MainUI does the browsing (Onion's `prompt` dies with signal 8
# on exit on this card, so its choice can't be read). Nothing uses the network except
# check and update.
#   menu.sh build | info <id> | check | update [id...]
D=$(cd "$(dirname "$0")" && pwd)
SD=/mnt/SDCARD
PATH=$SD/.tmp_update/bin:$PATH
T="Port manager"
TAB=$(printf '\t')
F="$SD/Roms/PORTS/Shortcuts/~Port manager"
UPDATES=$SD/.pm_work/updates.txt

busy() {
	rm -f /tmp/dismiss_info_panel
	infoPanel -t "$T" -m "$1" --persistent &
}
unbusy() {
	touch /tmp/dismiss_info_panel
	sync
	sleep 1
}

# show <title> <file>: the text in panels of up to ~230 characters (B on a panel: stop)
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
		infoPanel -t "$t" -m "$page" < /dev/null
		[ $? -eq 255 ] && break
	done < /tmp/pm_pages.txt
}

# entry <folder> <label> <menu.sh arguments>: a .miyoocmd named after the label
entry() {
	f=$(echo "$2" | tr '/\\:*?"<>|' '---------')
	printf '#!/bin/sh\nexec sh "%s/menu.sh" %s\n' "$D" "$3" > "$1/$f.miyoocmd"
}

build() {
	case "$F" in /mnt/SDCARD/Roms/PORTS/Shortcuts/~Port\ manager) ;; *) echo "bad menu folder: $F"; exit 1 ;; esac
	sh "$D/pm.sh" list > /tmp/pm_list.txt 2>> /tmp/pm_menu.log
	sh "$D/pm.sh" all > /tmp/pm_all.txt 2>> /tmp/pm_menu.log
	rm -rf "$F.new"
	mkdir -p "$F.new/~All ports in the catalog"
	while IFS="$TAB" read -r id label; do entry "$F.new" "$label" "info $id"; done < /tmp/pm_list.txt
	while IFS="$TAB" read -r id label; do entry "$F.new/~All ports in the catalog" "$label" "info $id"; done < /tmp/pm_all.txt
	entry "$F.new" "~Check for updates" check
	entry "$F.new" "~Refresh list" build
	if [ -s "$UPDATES" ]; then
		entry "$F.new" "~Update all ($(wc -l < "$UPDATES"))" update
		while read -r _ id rest; do
			entry "$F.new" "~Update $(echo "$rest" | sed 's/: / /; s/ -> / to /')" "update $id"
		done < "$UPDATES"
	fi
	rm -rf "$F"
	mv "$F.new" "$F"
	# Onion's cached lists of these folders
	rm -f "$SD/Roms/PORTS/Shortcuts/"*_cache*.db
	sync
}

case "$1" in
build)
	build ;;
info)
	sh "$D/pm.sh" info "$2" > /tmp/pm_info.txt 2>&1
	show "$T" /tmp/pm_info.txt ;;
check)
	busy "Checking for updates..."
	sh "$D/pm.sh" check > /tmp/pm_check.txt 2>&1
	grep '^UPDATE ' /tmp/pm_check.txt > "$UPDATES"
	build
	unbusy
	if grep -q '^NONET ' /tmp/pm_check.txt; then
		{ echo "Can't check for updates:"; grep '^NONET ' /tmp/pm_check.txt | cut -d' ' -f2-; } > /tmp/pm_msg.txt
	elif grep -q '^ERROR' /tmp/pm_check.txt; then
		{ echo "Can't check:"; grep '^ERROR' /tmp/pm_check.txt | head -3 | cut -d' ' -f3-; echo "Is Wi-Fi on?"; } > /tmp/pm_msg.txt
	elif [ -s "$UPDATES" ]; then
		{ echo "Updates:"; cut -d' ' -f3- "$UPDATES"; echo "Choose ~Update all or one ~Update entry in the list."; } > /tmp/pm_msg.txt
	else
		echo "All installed ports are up to date." > /tmp/pm_msg.txt
	fi
	show "$T" /tmp/pm_msg.txt ;;
update)
	shift
	[ $# -gt 0 ] || set -- $(cut -d' ' -f2 "$UPDATES" 2>/dev/null)
	busy "Updating..."
	sh "$D/pm.sh" update "$@" > /tmp/pm_update.txt 2>&1
	# updated ports leave the update list
	grep '^DONE ' /tmp/pm_update.txt | cut -d' ' -f2 > /tmp/pm_done.txt
	[ -f "$UPDATES" ] && grep -vwFf /tmp/pm_done.txt "$UPDATES" > "$UPDATES.new"; mv -f "$UPDATES.new" "$UPDATES" 2>/dev/null
	build
	unbusy
	grep -E ' -> .*replaced|^ERROR|^NONET|kept your|^backup' /tmp/pm_update.txt | sed 's/^NONET /Can'"'"'t update: /' > /tmp/pm_msg.txt
	[ -s /tmp/pm_msg.txt ] || echo "Nothing was updated." > /tmp/pm_msg.txt
	show "$T" /tmp/pm_msg.txt ;;
*)
	sed -n '2,9s/^# \{0,1\}//p' "$0" ;;
esac
