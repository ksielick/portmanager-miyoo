#!/bin/sh
# Port manager for OnionOS on the Miyoo Mini: checks and updates the ports listed in
# catalog.json, downloading each one from its author's GitHub release. Everything stays in
# Onion's Ports list (Roms/PORTS/Games, Roms/PORTS/Shortcuts/<Category>, Roms/PORTS/Imgs),
# never App/. Docs: https://github.com/ksielick/portmaster-miyoo
#   pm.sh list             installed ports, "id<TAB>label" (no network)
#   pm.sh all              every port in the catalog, "id<TAB>label" (no network)
#   pm.sh info <id>        a port's details and how to add its game files (no network)
#   pm.sh check            installed ports: version, latest release, missing game data
#   pm.sh update [id...]   installed ports with a newer release (all, or the ids given;
#                          an id given by name is updated even when its version is unknown)
# check and update first refresh catalog.json from GitHub; list, all and info use the copy.
# PM_DRY=1: update only says what it would do. PM_SD: the card (default /mnt/SDCARD).
# PM_CATALOG: a catalog file to use as is (default: catalog.json here, refreshed from GitHub).
# Rules: only files from the release are written; the player's files (game data, saves,
# settings) are never touched; a game's `backup` folders and every replaced file go to
# Backups/PortManager/<date>-<id>-before-<tag>/ first; a shortcut or picture the player
# changed is kept. Log: log.txt next to this script.
# jq takes ~0.5 s to start on the Miyoo, so it runs once for the catalog and once per release
# (fetched in parallel); everything else reads the .tsv files it leaves in $WORK.
SD=${PM_SD:-/mnt/SDCARD}
HERE=$(cd "$(dirname "$0")" && pwd)
CAT=${PM_CATALOG:-$HERE/catalog.json}
CAT_URL=https://raw.githubusercontent.com/ksielick/portmaster-miyoo/main/catalog.json
SC=$SD/Roms/PORTS/Shortcuts
WORK=$SD/.pm_work
LOG=$HERE/log.txt
STAMP=$(date +%Y%m%d-%H%M%S)
TAB=$(printf '\t')
US=$(printf '\037')
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

# load_catalog [refresh]: the catalog (with refresh: first fetched from GitHub, unless
# PM_CATALOG is set) as catalog.tsv: one line per port, fields separated by \037, lists
# joined by |
load_catalog() {
	if [ -n "$1" ] && [ -z "$PM_CATALOG" ]; then
		if fetch "$CAT_URL" "$WORK/catalog.json" && ! cmp -s "$WORK/catalog.json" "$CAT"; then
			cp "$WORK/catalog.json" "$CAT"
			NEWCAT=1
		fi
	fi
	jq -r '.ports[] | [.id, .name, .dir, .layout, .repo, (.asset // ""), (.category // ""),
		((.skip // []) | join("|")), ((.backup // []) | join("|")),
		((.data.required // []) | join("|")), ((.data.required_any // []) | join("|")),
		(.data.rom.dest // ""), (.shortcut.GameDataFile // ""), (.shortcut.run // "")]
		| join("\u001f")' "$CAT" > "$WORK/catalog.tsv" 2>> "$LOG" && [ -s "$WORK/catalog.tsv" ] ||
		{ fail "catalog: can't read $CAT"; exit 1; }
	[ -n "$NEWCAT" ] && say "catalog updated"
}

ids() { cut -d"$US" -f1 "$WORK/catalog.tsv"; }

# entry <id>: the port's fields as shell variables
entry() {
	line=$(grep "^$1$US" "$WORK/catalog.tsv" | head -1)
	[ -n "$line" ] || return 1
	o=$IFS
	IFS=$US
	set -f
	set -- $line
	IFS=$o
	set +f
	id=$1 name=$2 dir=$3 layout=$4 repo=$5 asset=$6 category=$7 skips=$8 backups=$9
	shift 9
	required=$1 required_any=$2 rom=$3 sc_gdf=$4 sc_run=$5
	G=$SD/$dir
	base=$(basename "$dir")
}

version_of() { cat "$G/.pm_version" 2>/dev/null || cat "$G/.miyoo64_version" 2>/dev/null; }

# installed: the game folder has files (or, for app-to-ports, an old App/ copy)
installed() {
	[ -n "$(ls -A "$G" 2>/dev/null)" ] && return 0
	[ "$layout" = app-to-ports ] && [ -f "$SD/App/$base/launch.sh" ]
}

rel_file() { echo "$WORK/release_$(echo "$1" | tr '/' '_').tsv"; }

# fetch_releases <repo>...: each repo's latest release, in parallel, as release_<repo>.tsv:
# the tag, then "name\037url\037digest\037size" per file
fetch_releases() {
	for r in $(echo "$@" | tr ' ' '\n' | sort -u); do
		(
			f=$(rel_file "$r")
			fetch "https://api.github.com/repos/$r/releases/latest" "$f.json" &&
				jq -r '.tag_name, (.assets[] | [.name, .browser_download_url, (.digest // ""),
					(.size | tostring)] | join("\u001f"))' "$f.json" > "$f.part" 2>> "$LOG" &&
				[ -s "$f.part" ] && mv "$f.part" "$f"
			rm -f "$f.json" "$f.part"
		) &
	done
	wait
}

# has <folder> <path, case-insensitive, * allowed>
has() { find "$1" -maxdepth 4 -ipath "$1/${2%/}" 2>/dev/null | grep -q .; }

# data_missing: one line per missing game file of the current entry
data_missing() {
	set -f
	o=$IFS
	IFS='|'
	for p in $required; do has "$G" "$p" || echo "$p"; done
	if [ -n "$required_any" ]; then
		ok=
		for p in $required_any; do has "$G" "$p" && ok=1; done
		[ -n "$ok" ] || echo "one of: $(echo "$required_any" | sed 's/|/, /g')"
	fi
	IFS=$o
	set +f
	if [ -n "$rom" ] && [ ! -f "$G/$rom" ]; then echo "$rom"; fi
}

# installed entries (as "id" lines) and their repos
scan() {
	: > "$WORK/installed"
	repos=
	for i in $(ids); do
		entry "$i"
		installed || continue
		echo "$id" >> "$WORK/installed"
		[ -n "$asset" ] && repos="$repos $repo"
	done
}

cmd_check() {
	scan
	fetch_releases $repos
	for i in $(cat "$WORK/installed"); do
		entry "$i"
		cur=$(version_of)
		r=$(rel_file "$repo")
		if [ -z "$asset" ]; then
			say "NOREL $id $name: ${cur:-version unknown}, no releases yet"
		elif [ ! -s "$r" ]; then
			say "ERROR $id $name: no release info (no connection?)"
		else
			tag=$(head -1 "$r")
			if [ "$cur" = "$tag" ]; then
				say "OK $id $name: $tag"
			elif [ -z "$cur" ]; then
				say "UNKNOWN $id $name: version unknown, latest $tag"
			else
				say "UPDATE $id $name: $cur -> $tag"
			fi
		fi
		miss=$(data_missing)
		[ -n "$miss" ] && say "MISSING $id $name: $(echo "$miss" | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
	done
}

cmd_list() {
	for i in $(ids); do
		entry "$i"
		installed || continue
		v=$(version_of)
		m=$(data_missing)
		echo "$id$TAB$name (${v:-version unknown}${m:+, game files missing})"
	done | sort -f -t"$TAB" -k2
}

cmd_all() {
	for i in $(ids); do
		entry "$i"
		if installed; then
			v=$(version_of)
			s="installed${v:+ $v}${v:-, version unknown}"
		elif [ -z "$asset" ]; then
			s="no release yet"
		else
			s="not installed"
		fi
		echo "$id$TAB$category - $name ($s)"
	done | sort -f -t"$TAB" -k2
}

cmd_info() {
	entry "$1" || { echo "$1: not in the catalog"; return 1; }
	echo "$name ($category)"
	if installed; then
		v=$(version_of)
		echo "Installed: ${v:-version unknown}, in $dir"
		m=$(data_missing)
		if [ -n "$m" ]; then
			echo "Game files missing: $(echo "$m" | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
		else
			echo "Game files: OK"
		fi
	else
		echo "Not installed (would go to $dir)"
	fi
	if [ -n "$asset" ]; then echo "From: github.com/$repo"; else echo "From: github.com/$repo (no releases yet)"; fi
	jq -r --arg id "$1" '.ports[] | select(.id == $id) | .get // empty' "$CAT"
}

# map_files <stage> > map: one "kind<TAB>source<TAB>destination" line per file to install
# (source absolute, destination relative to the card)
map_files() {
	(cd "$1" && find . -type f) | sed 's#^\./##' | while IFS= read -r p; do
		skip=
		set -f
		o=$IFS
		IFS='|'
		for s in $skips; do
			case "$p" in $s*) skip=1 ;; esac
		done
		IFS=$o
		set +f
		[ -n "$skip" ] && continue
		case "$layout/$p" in
		"app-to-ports/App/$base/config.json") ;;
		"app-to-ports/App/$base/"*) echo "game$TAB$1/$p$TAB$dir/${p#App/$base/}" ;;
		*/Roms/PORTS/Imgs/*) echo "img$TAB$1/$p$TAB$p" ;;
		app-to-ports/Roms/PORTS/Shortcuts/*.port) echo "template$TAB$1/$p$TAB" ;;
		sdcard-root/Roms/PORTS/Shortcuts/*.port)
			n=$(basename "$p")
			to=$(find "$SC" -name "$n" -type f 2>/dev/null | head -1)
			[ -n "$to" ] || to=$SC/$category/$n
			echo "shortcut$TAB$1/$p$TAB${to#$SD/}" ;;
		sdcard-root/Roms/PORTS/Games/*) echo "game$TAB$1/$p$TAB$p" ;;
		esac
	done
}

# shortcut_from <release shortcut> > our shortcut: the release's settings (GameName,
# FullResolution, ...) without its App/ paths, then our GameDir and launch
shortcut_from() {
	echo '#!/bin/sh'
	echo "# $(basename "$1" .port): runs $dir/$sc_run. Written by Port manager (portmaster-miyoo)."
	grep -E '^[A-Za-z_][A-Za-z0-9_]*=' "$1" | grep -vE '^(APP|GameDir|GameDataFile)=' | grep -v 'App/'
	echo "GameDir=\"$base\""
	echo "GameDataFile=\"$sc_gdf\""
	echo "cd \"$G\" || exit 1"
	echo "exec sh \"$G/$sc_run\""
}

# backup_saves: the entry's backup folders, once, before its first replaced file
backup_saves() {
	[ -n "$saved" ] && return
	saved=1
	set -f
	o=$IFS
	IFS='|'
	for b in $backups; do
		[ -d "$G/$b" ] || continue
		mkdir -p "$BK/$dir"
		cp -r "$G/${b%/}" "$BK/$dir/" && say "$name: saved $dir/$b to the backup"
	done
	IFS=$o
	set +f
}

# update_one <id> [byname]
update_one() {
	entry "$1" || { fail "$1: not in the catalog"; return 1; }
	[ -n "$asset" ] || { say "$name: no releases yet"; return 0; }
	installed || { say "$name: not installed"; return 0; }
	r=$(rel_file "$repo")
	[ -s "$r" ] || { fail "$name: no release info (no connection?)"; return 1; }
	tag=$(head -1 "$r")
	cur=$(version_of)
	[ "$cur" = "$tag" ] && { say "$name: $tag, up to date"; return 0; }
	[ -z "$cur" ] && [ -z "$2" ] && { say "$name: version unknown, latest $tag (update it by name)"; return 0; }

	aname=$(tail -n +2 "$r" | cut -d"$US" -f1 | grep -E "$asset" | head -1)
	[ -n "$aname" ] || { fail "$name: $tag has no file matching $asset"; return 1; }
	a=$(grep -F "$aname$US" "$r" | head -1)
	url=$(echo "$a" | cut -d"$US" -f2)
	digest=$(echo "$a" | cut -d"$US" -f3)
	size=$(echo "$a" | cut -d"$US" -f4)
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

	map_files "$ST" > "$WORK/map"
	tpl=$(grep "^template$TAB" "$WORK/map" | head -1 | cut -f2)
	if [ -n "$tpl" ]; then
		shortcut_from "$tpl" > "$WORK/shortcut.port"
		to=$(grep -rl "^GameDir=\"$base\"" "$SC" 2>/dev/null | head -1)
		[ -n "$to" ] || to=$SC/$category/$(basename "$tpl")
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
	echo "DONE $id"
}

cmd_update() {
	scan
	if [ $# -gt 0 ]; then
		repos=
		for i in "$@"; do entry "$i" && repos="$repos $repo"; done
		fetch_releases $repos
		for i in "$@"; do update_one "$i" byname; done
	else
		fetch_releases $repos
		for i in $(cat "$WORK/installed"); do update_one "$i"; done
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
rm -f "$WORK"/release_*
echo "--- $(date) pm.sh $*" >> "$LOG"
[ -f "$CAT" ] || { fail "no catalog: $CAT"; exit 1; }
cmd=$1
[ $# -gt 0 ] && shift
case "$cmd" in
list) load_catalog; cmd_list ;;
all) load_catalog; cmd_all ;;
info) load_catalog; cmd_info "$1" ;;
check) load_catalog refresh; cmd_check ;;
update) load_catalog refresh; cmd_update "$@" ;;
*) sed -n '2,20s/^# \{0,1\}//p' "$0"; exit 1 ;;
esac
sync
