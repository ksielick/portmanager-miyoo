#!/bin/sh
# Port manager for OnionOS on the Miyoo Mini: checks and updates the ports listed in
# catalog.json, downloading each one from its author's GitHub release. Everything stays in
# Onion's Ports list (Roms/PORTS/Games, Roms/PORTS/Shortcuts/<Category>, Roms/PORTS/Imgs),
# never App/. Docs: https://github.com/ksielick/portmaster-miyoo
#   pm.sh check            installed ports: version, latest release, missing game data
#   pm.sh update [id...]   installed ports with a newer release (all, or the ids given;
#                          an id given by name is updated even when its version is unknown)
# PM_DRY=1: update only says what it would do. PM_SD: the card (default /mnt/SDCARD).
# PM_CATALOG: a catalog file to use as is (default: catalog.json here, refreshed from GitHub).
# Rules: only files from the release are written; the player's files (game data, saves,
# settings) are never touched; a game's `backup` folders and every replaced file go to
# Backups/PortManager/<date>-<id>-before-<tag>/ first; a shortcut or picture the player
# changed is kept. Log: log.txt next to this script.
SD=${PM_SD:-/mnt/SDCARD}
HERE=$(cd "$(dirname "$0")" && pwd)
CAT=${PM_CATALOG:-$HERE/catalog.json}
CAT_URL=https://raw.githubusercontent.com/ksielick/portmaster-miyoo/main/catalog.json
SC=$SD/Roms/PORTS/Shortcuts
WORK=$SD/.pm_work
LOG=$HERE/log.txt
STAMP=$(date +%Y%m%d-%H%M%S)
TAB=$(printf '\t')
NL='
'
PATH=$SD/.tmp_update/bin:$PATH

say() { echo "$*"; echo "$*" >> "$LOG"; }
fail() { say "ERROR $*"; }

# fetch <url> <file>: nothing is left behind on failure
fetch() {
	rm -f "$2.part"
	wget -q -T 30 -O "$2.part" "$1" 2>> "$LOG" && [ -s "$2.part" ] && mv "$2.part" "$2" && return 0
	rm -f "$2.part"
	return 1
}

# field <id> <jq expression on the entry>
field() { jq -r --arg id "$1" ".ports[] | select(.id == \$id) | $2 // empty" "$CAT"; }

refresh_catalog() {
	[ -n "$PM_CATALOG" ] && return
	if fetch "$CAT_URL" "$WORK/catalog.json" &&
		jq -e '.ports | length > 0' "$WORK/catalog.json" > /dev/null 2>&1; then
		cmp -s "$WORK/catalog.json" "$CAT" || { cp "$WORK/catalog.json" "$CAT" && say "catalog updated"; }
	else
		say "catalog: no connection, using the copy on the card"
	fi
}

# release <owner/repo>: prints the file with its latest release (fetched once per run)
release() {
	r=$WORK/release_$(echo "$1" | tr '/' '_').json
	[ -s "$r" ] || fetch "https://api.github.com/repos/$1/releases/latest" "$r" || return 1
	jq -e .tag_name "$r" > /dev/null 2>&1 || { rm -f "$r"; return 1; }
	echo "$r"
}

version_of() { cat "$SD/$1/.pm_version" 2>/dev/null || cat "$SD/$1/.miyoo64_version" 2>/dev/null; }

# installed <dir> <layout>: the game folder has files (or, for app-to-ports, an old App/ copy)
installed() {
	[ -n "$(ls -A "$SD/$1" 2>/dev/null)" ] && return 0
	[ "$2" = app-to-ports ] && [ -f "$SD/App/$(basename "$1")/launch.sh" ]
}

# has <folder> <path, case-insensitive, * allowed>
has() { find "$1" -maxdepth 4 -ipath "$1/${2%/}" 2>/dev/null | grep -q .; }

# data_missing <id> <dir>: one line per missing (or unwanted) game file
data_missing() {
	d=$SD/$2
	set -f
	old_ifs=$IFS
	IFS=$NL
	for p in $(field "$1" '.data.required[]?'); do has "$d" "$p" || echo "$p"; done
	any=$(field "$1" '.data.required_any[]?')
	if [ -n "$any" ]; then
		ok=
		for p in $any; do has "$d" "$p" && ok=1; done
		[ -n "$ok" ] || echo "one of: $(echo "$any" | tr '\n' ',' | sed 's/,$//; s/,/, /g')"
	fi
	for p in $(field "$1" '.data.forbidden[]?'); do has "$d" "$p" && echo "remove $p"; done
	IFS=$old_ifs
	set +f
	rom=$(field "$1" '.data.rom.dest')
	[ -n "$rom" ] && [ ! -f "$d/$rom" ] && echo "$rom"
}

cmd_check() {
	for id in $(jq -r '.ports[].id' "$CAT"); do
		name=$(field "$id" .name)
		dir=$(field "$id" .dir)
		installed "$dir" "$(field "$id" .layout)" || continue
		cur=$(version_of "$dir")
		if [ -z "$(field "$id" .asset)" ]; then
			say "NOREL $id $name: ${cur:-version unknown}, no releases yet"
		elif ! rel=$(release "$(field "$id" .repo)"); then
			say "ERROR $id $name: no release info (no connection?)"
		else
			tag=$(jq -r .tag_name "$rel")
			if [ "$cur" = "$tag" ]; then
				say "OK $id $name: $tag"
			elif [ -z "$cur" ]; then
				say "UNKNOWN $id $name: version unknown, latest $tag"
			else
				say "UPDATE $id $name: $cur -> $tag"
			fi
		fi
		miss=$(data_missing "$id" "$dir")
		[ -n "$miss" ] && say "MISSING $id $name: $(echo "$miss" | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
	done
}

# map_files <id> <stage> <dir> <layout> > map: one "kind<TAB>source<TAB>destination" line
# per file to install (source absolute, destination relative to the card)
map_files() {
	base=$(basename "$3")
	skips=$(field "$1" '.skip[]?')
	(cd "$2" && find . -type f) | sed 's#^\./##' | while IFS= read -r p; do
		skip=
		set -f
		for s in $skips; do
			case "$p" in $s*) skip=1 ;; esac
		done
		set +f
		[ -n "$skip" ] && continue
		case "$4/$p" in
		"app-to-ports/App/$base/config.json") ;;
		"app-to-ports/App/$base/"*) echo "game$TAB$2/$p$TAB$3/${p#App/$base/}" ;;
		*/Roms/PORTS/Imgs/*) echo "img$TAB$2/$p$TAB$p" ;;
		app-to-ports/Roms/PORTS/Shortcuts/*.port) echo "template$TAB$2/$p$TAB" ;;
		sdcard-root/Roms/PORTS/Shortcuts/*.port)
			n=$(basename "$p")
			to=$(find "$SC" -name "$n" -type f 2>/dev/null | head -1)
			[ -n "$to" ] || to=$SC/$(field "$1" .category)/$n
			echo "shortcut$TAB$2/$p$TAB${to#$SD/}" ;;
		sdcard-root/Roms/PORTS/Games/*) echo "game$TAB$2/$p$TAB$p" ;;
		esac
	done
}

# shortcut_from <release shortcut> <dir> <id> > our shortcut: the release's settings
# (GameName, FullResolution, ...) without its App/ paths, then our GameDir and launch
shortcut_from() {
	echo '#!/bin/sh'
	echo "# $(basename "$1" .port): runs $2/$(field "$3" .shortcut.run). Written by Port manager (portmaster-miyoo)."
	grep -E '^[A-Za-z_][A-Za-z0-9_]*=' "$1" | grep -vE '^(APP|GameDir|GameDataFile)=' | grep -v 'App/'
	echo "GameDir=\"$(basename "$2")\""
	echo "GameDataFile=\"$(field "$3" .shortcut.GameDataFile)\""
	echo "cd \"$SD/$2\" || exit 1"
	echo "exec sh \"$SD/$2/$(field "$3" .shortcut.run)\""
}

# backup_saves: the entry's backup folders, once, before its first replaced file
backup_saves() {
	[ -n "$saved" ] && return
	saved=1
	set -f
	for b in $(field "$id" '.backup[]?'); do
		[ -d "$G/$b" ] || continue
		mkdir -p "$BK/$dir"
		cp -r "$G/${b%/}" "$BK/$dir/" && say "$name: saved $dir/$b to the backup"
	done
	set +f
}

update_one() {
	id=$1
	name=$(field "$id" .name)
	[ -n "$name" ] || { fail "$id: not in the catalog"; return 1; }
	dir=$(field "$id" .dir)
	layout=$(field "$id" .layout)
	re=$(field "$id" .asset)
	G=$SD/$dir
	base=$(basename "$dir")
	[ -n "$re" ] || { say "$name: no releases yet"; return 0; }
	installed "$dir" "$layout" || { say "$name: not installed"; return 0; }
	rel=$(release "$(field "$id" .repo)") || { fail "$name: no release info (no connection?)"; return 1; }
	tag=$(jq -r .tag_name "$rel")
	cur=$(version_of "$dir")
	[ "$cur" = "$tag" ] && { say "$name: $tag, up to date"; return 0; }
	[ -z "$cur" ] && [ -z "$2" ] && { say "$name: version unknown, latest $tag (update it by name)"; return 0; }

	a=$(jq -r --arg re "$re" '[.assets[] | select(.name | test($re))][0] // empty |
		[.name, .browser_download_url, (.digest // ""), (.size | tostring)] | @tsv' "$rel")
	[ -n "$a" ] || { fail "$name: $tag has no file matching $re"; return 1; }
	aname=$(echo "$a" | cut -f1)
	url=$(echo "$a" | cut -f2)
	digest=$(echo "$a" | cut -f3)
	size=$(echo "$a" | cut -f4)
	z=$WORK/$aname
	say "$name: ${cur:-?} -> $tag, downloading $aname ($((size / 1024)) KB)"
	fetch "$url" "$z" || { fail "$name: download failed"; return 1; }
	case "$digest" in
	sha256:*)
		[ "$(sha256sum < "$z" | cut -d' ' -f1)" = "${digest#sha256:}" ] ||
			{ fail "$name: $aname: wrong sha256"; rm -f "$z"; return 1; } ;;
	*)
		[ "$(wc -c < "$z")" -eq "$size" ] || { fail "$name: $aname: wrong size"; rm -f "$z"; return 1; } ;;
	esac

	ST=$WORK/stage
	rm -rf "$ST"
	mkdir -p "$ST"
	case "$aname" in
	*.zip) unzip -q -o "$z" -d "$ST" ;;
	*.7z) 7z x -y -o"$ST" "$z" > /dev/null ;;
	*.tar.gz | *.tgz) tar -xzf "$z" -C "$ST" ;;
	*.tar) tar -xf "$z" -C "$ST" ;;
	*) false ;;
	esac || { fail "$name: can't unpack $aname"; return 1; }
	rm -f "$z"

	BK=$SD/Backups/PortManager/$STAMP-$id-before-$tag
	saved=

	# an older install in App/ (miyoo64's own layout): moved to the Ports folder first
	if [ "$layout" = app-to-ports ] && [ -f "$SD/App/$base/launch.sh" ]; then
		if [ -e "$G" ]; then
			fail "$name: both App/$base and $dir exist: sort it out by hand"
			return 1
		fi
		if [ -n "$PM_DRY" ]; then
			say "$name: would move App/$base -> $dir"
		else
			mv "$SD/App/$base" "$G" || { fail "$name: can't move App/$base"; return 1; }
			mkdir -p "$BK/App/$base"
			[ -f "$G/config.json" ] && mv "$G/config.json" "$BK/App/$base/"
			grep -rl "/App/$base\$" "$SC" 2>/dev/null | while IFS= read -r old; do
				mkdir -p "$BK/Shortcuts"
				mv "$old" "$BK/Shortcuts/"
			done
			say "$name: moved App/$base -> $dir"
		fi
	fi

	map_files "$id" "$ST" "$dir" "$layout" > "$WORK/map"
	tpl=$(grep "^template$TAB" "$WORK/map" | head -1 | cut -f2)
	if [ -n "$tpl" ]; then
		shortcut_from "$tpl" "$dir" "$id" > "$WORK/shortcut.port"
		to=$(grep -rl "^GameDir=\"$base\"" "$SC" 2>/dev/null | head -1)
		[ -n "$to" ] || to=$SC/$(field "$id" .category)/$(basename "$tpl")
		echo "shortcut$TAB$WORK/shortcut.port$TAB${to#$SD/}" >> "$WORK/map"
	fi

	MAN=$G/.pm_manifest
	: > "$WORK/manifest"
	n=0
	while IFS="$TAB" read -r kind src dst; do
		[ "$kind" = template ] && continue
		new=$(md5sum < "$src" | cut -d' ' -f1)
		D=$SD/$dst
		if [ -f "$D" ]; then
			old=$(md5sum < "$D" | cut -d' ' -f1)
			if [ "$old" = "$new" ]; then
				echo "$new $dst" >> "$WORK/manifest"
				continue
			fi
			if [ "$kind" != game ]; then
				ours=
				grep -qFx "$old $dst" "$MAN" 2>/dev/null && ours=1
				[ "$kind" = shortcut ] && grep -qE 'Written by (update_miyoo64\.sh|Port manager)' "$D" && ours=1
				if [ -z "$ours" ]; then
					say "$name: kept your $dst"
					continue
				fi
			fi
		fi
		if [ -n "$PM_DRY" ]; then
			say "$name: would write $dst"
			n=$((n + 1))
			continue
		fi
		backup_saves
		if [ -f "$D" ]; then
			mkdir -p "$BK/$(dirname "$dst")"
			cp "$D" "$BK/$dst"
		fi
		mkdir -p "$(dirname "$D")"
		cp "$src" "$D.new" && mv "$D.new" "$D" || { fail "$name: can't write $dst"; return 1; }
		[ "$kind" = game ] && chmod +x "$D" 2> /dev/null
		[ "$kind" = shortcut ] && shortcuts_changed=1
		echo "$new $dst" >> "$WORK/manifest"
		say "$name: updated $dst"
		n=$((n + 1))
	done < "$WORK/map"
	rm -rf "$ST"

	if [ -n "$PM_DRY" ]; then
		say "$name: ${cur:-?} -> $tag would replace $n file(s) (dry run)"
		return 0
	fi
	cp "$WORK/manifest" "$MAN"
	echo "$tag" > "$G/.pm_version"
	[ -f "$G/.miyoo64_version" ] && echo "$tag" > "$G/.miyoo64_version"
	say "$name: ${cur:-?} -> $tag, $n file(s) replaced"
}

cmd_update() {
	if [ $# -gt 0 ]; then
		for id in "$@"; do update_one "$id" byname; done
	else
		for id in $(jq -r '.ports[].id' "$CAT"); do
			installed "$(field "$id" .dir)" "$(field "$id" .layout)" && update_one "$id"
		done
	fi
	# Onion's cached Ports list (rebuilt when the list opens)
	[ -n "$shortcuts_changed" ] && rm -f "$SC"/Shortcuts_cache*.db
	ls -d "$SD/Backups/PortManager/$STAMP"-* 2> /dev/null | sed 's/^/backup: /'
}

mkdir -p "$WORK"
if ! mkdir "$WORK/lock" 2> /dev/null; then
	echo "ERROR the port manager is already running (or $WORK/lock was left behind)"
	exit 1
fi
trap 'rm -rf "$WORK/lock" "$WORK/stage"' EXIT
echo "--- $(date) pm.sh $*" >> "$LOG"
[ -f "$CAT" ] || { fail "no catalog: $CAT"; exit 1; }
cmd=$1
[ $# -gt 0 ] && shift
case "$cmd" in
check) refresh_catalog; cmd_check ;;
update) refresh_catalog; cmd_update "$@" ;;
*) sed -n '2,13s/^# \{0,1\}//p' "$0"; exit 1 ;;
esac
sync
