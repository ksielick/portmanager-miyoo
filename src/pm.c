#include "pm.h"
#include "gfx.h"

#include <dirent.h>
#include <fnmatch.h>
#include <net/if.h>
#include <netinet/in.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "vendor/cJSON.h"
#include "vendor/miniz.h"

#define CAT_URL "https://raw.githubusercontent.com/ksielick/portmanager-miyoo/main/catalog.json"
#define API "https://api.github.com/repos/"

Port *ports;
int nports;
char pm_error[512];
int pm_folder_kept;
int pm_no_catalog;

int pm_self_update_available = 0;
char pm_self_latest_version[64] = {0};
char pm_self_asset_url[512] = {0};
char pm_self_asset_name[128] = {0};
long pm_self_asset_size = 0;

static char SD[256], HERE[512], CAT[600], WORK[300], SC[300], STAMP[32];

static int shortcuts_changed;

typedef struct {
	char pattern[128];
	char token[256];
} TokenEntry;

static TokenEntry tokens[32];
static int ntokens;

static void tokens_load(const char *path)
{
	char *t = read_file(path, NULL);
	if (!t)
		return;
	for (char *line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#')
			continue;
		char *eq = strchr(line, '=');
		if (eq) {
			*eq = 0;
			char *pat = line;
			char *val = eq + 1;
			while (*pat && (pat[strlen(pat) - 1] == ' ' || pat[strlen(pat) - 1] == '\t'))
				pat[strlen(pat) - 1] = 0;
			while (*val == ' ' || *val == '\t')
				val++;
			while (*val && (val[strlen(val) - 1] == ' ' || val[strlen(val) - 1] == '\t'))
				val[strlen(val) - 1] = 0;
			if (*pat && *val && ntokens < 32) {
				snprintf(tokens[ntokens].pattern, sizeof tokens[ntokens].pattern, "%s", pat);
				snprintf(tokens[ntokens].token, sizeof tokens[ntokens].token, "%s", val);
				ntokens++;
			}
		} else if (ntokens < 32) {
			/* Single line with token = default wildcard */
			snprintf(tokens[ntokens].pattern, sizeof tokens[ntokens].pattern, "*");
			snprintf(tokens[ntokens].token, sizeof tokens[ntokens].token, "%s", line);
			ntokens++;
		}
	}
	free(t);
}

static const char *token_for_repo(const char *repo)
{
	if (!repo || !*repo || ntokens == 0)
		return NULL;
	/* 1. Exact repo match: "owner/repo" */
	for (int i = 0; i < ntokens; i++)
		if (!strcasecmp(tokens[i].pattern, repo))
			return tokens[i].token;
	/* 2. Owner match: owner wildcard */
	const char *slash = strchr(repo, '/');
	if (slash) {
		char owner_wild[128];
		snprintf(owner_wild, sizeof owner_wild, "%.*s/*", (int)(slash - repo), repo);
		for (int i = 0; i < ntokens; i++)
			if (!strcasecmp(tokens[i].pattern, owner_wild))
				return tokens[i].token;
	}
	/* 3. Global wildcard "*" */
	for (int i = 0; i < ntokens; i++)
		if (!strcmp(tokens[i].pattern, "*"))
			return tokens[i].token;
	return NULL;
}

/* --- the catalog --- */

static char *str(const cJSON *o, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return strdup(cJSON_IsString(v) ? v->valuestring : "");
}

static char **strs(const cJSON *o, const char *key, int *n)
{
	const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key), *v;
	*n = 0;
	if (!a)
		return calloc(1, sizeof(char *));
	if (cJSON_IsString(a)) {
		char **r = calloc(2, sizeof *r);
		r[(*n)++] = strdup(a->valuestring);
		return r;
	}
	char **r = calloc(cJSON_GetArraySize(a) + 1, sizeof *r);
	cJSON_ArrayForEach(v, a)
		if (cJSON_IsString(v))
			r[(*n)++] = strdup(v->valuestring);
	return r;
}

static int is_safe_name(const char *s)
{
	if (!s || !*s)
		return 0;
	if (strstr(s, "..") || strchr(s, '/') || strchr(s, '\\') || strchr(s, '\r') || strchr(s, '\n'))
		return 0;
	return 1;
}

static int is_safe_rel_path(const char *s)
{
	if (!s || !*s)
		return 0;
	if (s[0] == '/' || s[0] == '\\' || strstr(s, "..") || strchr(s, '\\') || strchr(s, '\r') || strchr(s, '\n'))
		return 0;
	return 1;
}

static int validate_port(const Port *p)
{
	if (!is_safe_name(p->id) || !p->name || !*p->name)
		return 0;
	if (strstr(p->name, "..") || strchr(p->name, '/') || strchr(p->name, '\\'))
		return 0;
	if (*p->category && (!is_safe_name(p->category)))
		return 0;
	if (*p->app && !is_safe_name(p->app))
		return 0;
	if (strncmp(p->dir, "Roms/PORTS/Games/", 17) != 0)
		return 0;
	if (strstr(p->dir, "..") || strchr(p->dir, '\\') || strlen(p->dir) <= 17)
		return 0;
	if (*p->rom && !is_safe_rel_path(p->rom))
		return 0;
	if (*p->sc_run && !is_safe_rel_path(p->sc_run))
		return 0;
	for (int i = 0; i < p->nbackup; i++)
		if (!is_safe_rel_path(p->backup[i]))
			return 0;
	for (int i = 0; i < p->nrequired; i++)
		if (!is_safe_rel_path(p->required[i]))
			return 0;
	for (int i = 0; i < p->nrequired_any; i++)
		if (!is_safe_rel_path(p->required_any[i]))
			return 0;
	for (int i = 0; i < p->nbuilt; i++)
		if (!is_safe_rel_path(p->built[i]))
			return 0;
	for (int i = 0; i < p->noptional; i++)
		if (!is_safe_rel_path(p->optional[i]))
			return 0;
	return 1;
}

static void parse_port(const cJSON *e, Port *p)
{
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(e, "data");
	const cJSON *rom = cJSON_GetObjectItemCaseSensitive(data, "rom");
	const cJSON *sc = cJSON_GetObjectItemCaseSensitive(e, "shortcut");
	p->id = str(e, "id");
	p->name = str(e, "name");
	p->dir = str(e, "dir");
	p->layout = str(e, "layout");
	p->app = str(e, "app");
	p->repo = str(e, "repo");
	p->asset = str(e, "asset");
	p->category = str(e, "category");
	p->status = str(e, "status");
	p->get = str(e, "get");
	p->rom = str(rom, "dest");
	p->rom_name = str(rom, "name");
	p->sc_gdf = str(sc, "GameDataFile");
	p->sc_run = str(sc, "run");
	p->skip = strs(e, "skip", &p->nskip);
	p->backup = strs(e, "backup", &p->nbackup);
	p->required = strs(data, "required", &p->nrequired);
	p->required_any = strs(data, "required_any", &p->nrequired_any);
	p->built = strs(data, "built", &p->nbuilt);
	p->optional = strs(data, "optional", &p->noptional);
}

static cJSON *read_json(const char *path)
{
	char *text = read_file(path, NULL);
	cJSON *root = text ? cJSON_Parse(text) : NULL;
	free(text);
	return root;
}

/* the catalog, then catalog.local.json next to this program (the player's own entries, never
 * replaced by the catalog from GitHub); a local entry with a catalog entry's id replaces it.
 * No catalog file at all is fine (the check downloads it), a broken one is not. */
static int load_catalog(const char *path)
{
	char local_path[600];
	snprintf(local_path, sizeof local_path, "%s/catalog.local.json", HERE);
	int missing = !file_exists(path);
	cJSON *root = missing ? NULL : read_json(path), *local = NULL;
	const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "ports"), *llist = NULL, *e;
	if (!missing && (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0)) {
		cJSON_Delete(root);
		return -1;
	}
	if (file_exists(local_path)) {
		local = read_json(local_path);
		llist = cJSON_GetObjectItemCaseSensitive(local, "ports");
		if (!cJSON_IsArray(llist)) {
			say("catalog.local.json: not a valid catalog, ignored");
			llist = NULL;
		}
	}
	ports = calloc(cJSON_GetArraySize(list) + cJSON_GetArraySize(llist) + 1, sizeof *ports);
	nports = 0;
	const cJSON *lists[] = {list, llist};
	for (int l = 0; l < 2; l++)
		cJSON_ArrayForEach(e, lists[l]) {
			Port p = {0};
			parse_port(e, &p);
			if (!validate_port(&p)) {
				say("%s: rejected invalid/unsafe port entry %s", l ? "catalog.local.json" : "catalog",
					*p.id ? p.id : "(unnamed)");
				continue;
			}
			int k = 0;
			while (k < nports && strcmp(ports[k].id, p.id))
				k++;
			if (k < nports)
				say_log("%s: %s replaces an earlier entry", l ? "catalog.local.json" : "catalog", p.id);
			else
				nports++;
			ports[k] = p;
		}
	if (llist)
		say_log("catalog.local.json: %d entries", cJSON_GetArraySize(llist));
	cJSON_Delete(root);
	cJSON_Delete(local);
	if (missing) {
		say_log("catalog: none on the card yet");
		return 0;
	}
	if (nports == 0) {
		free(ports);
		ports = NULL;
		return -1;
	}
	return 0;
}

/* --- what's on the card --- */

static const char *gdir(const Port *p)
{
	static char g[600];
	snprintf(g, sizeof g, "%s/%s", SD, p->dir);
	return g;
}

static int dir_has_files(const char *d)
{
	DIR *dd = opendir(d);
	struct dirent *e;
	int any = 0;
	while (dd && !any && (e = readdir(dd)))
		any = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
	if (dd)
		closedir(dd);
	return any;
}

/* app-to-ports: the zip's folder under App/ */
static const char *app_name(const Port *p)
{
	return *p->app ? p->app : base_name(p->dir);
}

static int legacy_app(const Port *p)
{
	char a[600];
	snprintf(a, sizeof a, "%s/App/%s/launch.sh", SD, app_name(p));
	return !strcmp(p->layout, "app-to-ports") && file_exists(a);
}

/* has <dir> <path pattern>: case-insensitive, '*' allowed, a trailing '/' wants a folder */
static int has(const char *dir, const char *pat)
{
	const char *slash = strchr(pat, '/');
	char first[256];
	snprintf(first, sizeof first, "%.*s", slash ? (int)(slash - pat) : (int)strlen(pat), pat);
	const char *rest = slash ? slash + 1 : "";
	DIR *d = opendir(dir);
	struct dirent *e;
	int found = 0;
	while (d && !found && (e = readdir(d))) {
		if (e->d_name[0] == '.' && (!e->d_name[1] || !strcmp(e->d_name, "..")))
			continue;
		if (fnmatch(first, e->d_name, FNM_CASEFOLD))
			continue;
		char full[1024];
		snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
		if (!slash)
			found = 1;
		else if (!*rest)
			found = is_dir(full);
		else
			found = is_dir(full) && has(full, rest);
	}
	if (d)
		closedir(d);
	return found;
}

static int ends_with(const char *s, const char *suf)
{
	size_t sl = strlen(s), sufl = strlen(suf);
	return sl >= sufl && !strcasecmp(s + sl - sufl, suf);
}

static int is_code_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	unsigned char magic[4];
	size_t n = fread(magic, 1, 4, f);
	fclose(f);
	if (n >= 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F')
		return 1;
	if (n >= 2 && magic[0] == '#' && magic[1] == '!')
		return 1;
	return 0;
}

/* finds a shortcut under Shortcuts (any category): by file name, or by its GameDir line */
typedef struct {
	const char *name, *gamedir;
	char found[700];
} Find;

static void find_cb(const char *rel, void *ctx)
{
	Find *f = ctx;
	if (*f->found || !strstr(rel, ".port"))
		return;
	if (f->name && strcmp(base_name(rel), f->name))
		return;
	if (f->gamedir) {
		char full[1000], line[200];
		snprintf(full, sizeof full, "%s/%s", SC, rel);
		snprintf(line, sizeof line, "\nGameDir=\"%s\"", f->gamedir);
		char *t = read_file(full, NULL);
		int hit = t && strstr(t, line);
		free(t);
		if (!hit)
			return;
	}
	snprintf(f->found, sizeof f->found, "Roms/PORTS/Shortcuts/%s", rel);
}

static void find_shortcut(const Port *p, char *found, size_t size)
{
	found[0] = 0;
	Find f = {NULL, base_name(p->dir), ""};
	walk_files(SC, find_cb, &f);
	if (*f.found) {
		snprintf(found, size, "%s", f.found);
		return;
	}
	char bn[300];
	snprintf(bn, sizeof bn, "%s.port", p->name);
	Find f2 = {bn, NULL, ""};
	walk_files(SC, find_cb, &f2);
	if (*f2.found)
		snprintf(found, size, "%s", f2.found);
}

/* rel matches a catalog entry: a folder ("x/": everything in it), a path or a file name pattern */
static int in_list(char **list, int n, const char *rel)
{
	for (int i = 0; i < n; i++) {
		const char *e = list[i];
		size_t len = strlen(e);
		if (len > 0 && e[len - 1] == '/' && !strncasecmp(rel, e, len))
			return 1;
		if (!fnmatch(e, rel, FNM_CASEFOLD) || !fnmatch(e, base_name(rel), FNM_CASEFOLD))
			return 1;
	}
	return 0;
}

/* rel lies in a folder named dir (any letter case), at any depth */
static int has_dir(const char *rel, const char *dir)
{
	size_t len = strlen(dir);
	for (const char *s = rel; s; s = strchr(s, '/')) {
		if (*s == '/')
			s++;
		if (!strncasecmp(s, dir, len) && s[len] == '/')
			return 1;
	}
	return 0;
}

static int is_data_file(const Port *p, const char *rel)
{
	/* Metadata files are never data */
	if (!strcmp(rel, ".pm_version") || !strcmp(rel, ".miyoo64_version") || !strcmp(rel, ".pm_manifest"))
		return 0;

	/* What the catalog names as data (1, 3-7) comes before the guesses about code (on the
	 * card's FAT every file looks executable: a check of that once deleted F-Zero X's ROM and
	 * saves, 2026-10-17) */

	/* 1. ROM file */
	if (*p->rom && (!strcasecmp(rel, p->rom) || !strcasecmp(base_name(rel), p->rom)))
		return 1;

	/* 3-6. The catalog's data: required, required_any, optional, backup and built */
	if (in_list(p->required, p->nrequired, rel) || in_list(p->required_any, p->nrequired_any, rel) ||
	    in_list(p->optional, p->noptional, rel) || in_list(p->backup, p->nbackup, rel) ||
	    in_list(p->built, p->nbuilt, rel))
		return 1;

	/* 7. Standard save / data folder names, at any depth: OpenLara keeps its videos in
	 * "Tomb Raider 1/FMV/", and "remove, keep data" deleted them (2026-10-18) */
	static const char *dirs[] = {
		"data", "data1", "dc_data", "save", "saves", "music", "sound", "sounds",
		"movies", "fmv", "video", "videos"
	};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (has_dir(rel, dirs[i]))
			return 1;

	/* Scripts, libraries, and binaries are never data */
	if (ends_with(rel, ".sh") || ends_with(rel, ".so") || strstr(rel, ".so."))
		return 0;
	if (!strncmp(rel, "lib/", 4) || !strncmp(rel, "bin/", 4) || !strncmp(rel, "miyoo/", 6))
		return 0;

	char full[1024];
	snprintf(full, sizeof full, "%s/%s", gdir(p), rel);
	if (is_code_file(full))
		return 0;

	/* 2. Common ROM / CD image / save extensions */
	const char *exts[] = {
		".z64", ".n64", ".v64", ".iso", ".cue", ".chd", ".pbp", ".rom",
		".sav", ".srm", ".eep", ".fla", ".mpk", ".state", ".cfg", ".ini",
		".mp3", ".ogg", ".wav", ".mid", ".midi", ".flac",
		/* videos and game data files (OpenLara: the .RPL videos, savegame.dat) */
		".rpl", ".str", ".fmv", ".mp4", ".avi", ".bik", ".mpg", ".dat"
	};
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if (ends_with(rel, exts[i]))
			return 1;

	return 0;
}

typedef struct {
	const Port *p;
	int has_code;
} ProgramCheck;

static void check_prog_cb(const char *rel, void *ctx)
{
	ProgramCheck *pc = ctx;
	if (pc->has_code)
		return;
	if (!is_data_file(pc->p, rel))
		pc->has_code = 1;
}

static int has_program(const Port *p)
{
	if (legacy_app(p))
		return 1;
	const char *g = gdir(p);
	char f[700];
	snprintf(f, sizeof f, "%s/.pm_version", g);
	if (file_exists(f))
		return 1;
	snprintf(f, sizeof f, "%s/.miyoo64_version", g);
	if (file_exists(f))
		return 1;
	char sc[700];
	find_shortcut(p, sc, sizeof sc);
	if (*sc)
		return 1;
	ProgramCheck pc = {p, 0};
	walk_files(g, check_prog_cb, &pc);
	return pc.has_code;
}

static void read_version(Port *p)
{
	char f[700];
	p->version[0] = 0;
	const char *names[] = {".pm_version", ".miyoo64_version"};
	for (int i = 0; i < 2 && !p->version[0]; i++) {
		snprintf(f, sizeof f, "%s/%s", gdir(p), names[i]);
		char *v = read_file(f, NULL);
		if (v) {
			snprintf(p->version, sizeof p->version, "%s", v);
			p->version[strcspn(p->version, "\r\n")] = 0;
			free(v);
		}
	}
}

static void add_missing(Port *p, const char *what)
{
	size_t n = strlen(p->missing);
	snprintf(p->missing + n, sizeof p->missing - n, "%s%s", n ? "; " : "", what);
}

static void check_data(Port *p)
{
	const char *g = gdir(p);
	p->missing[0] = 0;
	for (int i = 0; i < p->nrequired; i++)
		if (!has(g, p->required[i]))
			add_missing(p, p->required[i]);
	if (p->nrequired_any) {
		int ok = 0;
		char any[256] = "one of: ";
		for (int i = 0; i < p->nrequired_any; i++) {
			ok |= has(g, p->required_any[i]);
			snprintf(any + strlen(any), sizeof any - strlen(any), "%s%s", i ? ", " : "", p->required_any[i]);
		}
		if (!ok)
			add_missing(p, any);
	}
	if (*p->rom) {
		char r[800];
		snprintf(r, sizeof r, "%s/%s", g, p->rom);
		if (!file_exists(r)) {
			int built_ok = 0;
			for (int i = 0; i < p->nbuilt; i++) {
				if (has(g, p->built[i])) {
					built_ok = 1;
					break;
				}
			}
			if (!built_ok)
				add_missing(p, p->rom);
		}
	}
}

/* "1.0.1" and "v1.0.1" are the same release (versions written by other tools lack the v) */
static int same_version(const char *a, const char *b)
{
	if (*a == 'v' || *a == 'V')
		a++;
	if (*b == 'v' || *b == 'V')
		b++;
	return !strcmp(a, b);
}

void pm_scan(void)
{
	int ninst = 0;
	for (int i = 0; i < nports; i++) {
		Port *p = &ports[i];
		p->installed = has_program(p);
		read_version(p);
		if (p->installed || dir_has_files(gdir(p))) {
			check_data(p);
			if (p->installed)
				ninst++;
		}
		else
			p->missing[0] = 0;
		p->update = p->installed && p->checked && p->asset_name[0] && *p->version && !same_version(p->version, p->latest);
	}
	say("--- scan: %d/%d ports installed", ninst, nports);
}

int pm_init(void)
{
	snprintf(SD, sizeof SD, "%s", getenv("PM_SD") ? getenv("PM_SD") : "/mnt/SDCARD");
	/* Onion's tools (wget, 7z) first */
	char path[1024];
	snprintf(path, sizeof path, "%s/.tmp_update/bin:%s", SD, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin:/usr/sbin:/sbin");
	setenv("PATH", path, 1);
	ssize_t n = readlink("/proc/self/exe", HERE, sizeof HERE - 1);
	HERE[n > 0 ? n : 0] = 0;
	char *s = strrchr(HERE, '/');
	if (s)
		*s = 0;
	snprintf(CAT, sizeof CAT, "%s/catalog.json", HERE);
	snprintf(WORK, sizeof WORK, "%s/.pm_work", SD);
	snprintf(SC, sizeof SC, "%s/Roms/PORTS/Shortcuts", SD);
	time_t t = time(NULL);
	strftime(STAMP, sizeof STAMP, "%Y%m%d-%H%M%S", localtime(&t));
	mkdirs(WORK);
	/* the only places Port Manager writes or deletes in: the Ports list, its backups, its
	 * work folder and its own folder (self-update); anything else is refused (owner, 2026-10-19) */
	const char *allowed[] = {"Roms/PORTS/Games", "Roms/PORTS/Shortcuts", "Roms/PORTS/Imgs", "Backups/PortManager"};
	for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++) {
		char d[600];
		snprintf(d, sizeof d, "%s/%s", SD, allowed[i]);
		guard_allow(d);
	}
	guard_allow(WORK);
	guard_allow(HERE);
	char log[600];
	snprintf(log, sizeof log, "%s/log.txt", HERE);
	log_open(log);
	say("init: SD=%s", SD);
	char cert[600];
	snprintf(cert, sizeof cert, "%s/cacert.pem", HERE);
	if (file_exists(cert)) {
		set_ca_cert(cert);
		say("tls: using CA bundle %s", cert);
	} else {
		snprintf(cert, sizeof cert, "%s/App/PortManager/cacert.pem", SD);
		if (file_exists(cert)) {
			set_ca_cert(cert);
			say("tls: using CA bundle %s", cert);
		}
	}
	char tokfile[600];
	snprintf(tokfile, sizeof tokfile, "%s/tokens.txt", HERE);
	if (!file_exists(tokfile))
		snprintf(tokfile, sizeof tokfile, "%s/App/PortManager/tokens.txt", SD);
	if (file_exists(tokfile)) {
		tokens_load(tokfile);
		say("auth: loaded %d token(s) from %s", ntokens, tokfile);
	}
	pm_no_catalog = !file_exists(CAT);
	if (load_catalog(CAT))
		return -1;
	pm_scan();
	return 0;
}

const char *pm_here(void)
{
	return HERE;
}

const char *pm_version(void)
{
	static char v[64];
	char f[600];
	snprintf(f, sizeof f, "%s/.pm_version", HERE);
	char *text = read_file(f, NULL);
	if (text) {
		char *p = text;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		char *end = p + strlen(p) - 1;
		while (end >= p && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
			*end-- = 0;
		if (*p) {
			snprintf(v, sizeof v, "%s", p);
			free(text);
			return v;
		}
		free(text);
	}
	return PM_VERSION;
}

int pm_lock(void)

{
	char lock[400], pidf[450];
	snprintf(lock, sizeof lock, "%s/lock", WORK);
	snprintf(pidf, sizeof pidf, "%s/pid", lock);
	if (mkdir(lock, 0777) != 0) {
		char *pid = read_file(pidf, NULL);
		int alive = pid && atoi(pid) > 0 && kill(atoi(pid), 0) == 0 && atoi(pid) != getpid();
		free(pid);
		if (alive)
			return -1;
	}
	char me[32];
	snprintf(me, sizeof me, "%d\n", getpid());
	return write_file(pidf, me, strlen(me));
}

void pm_unlock(void)
{
	char lock[400];
	snprintf(lock, sizeof lock, "%s/lock", WORK);
	remove_tree(lock);
}

/* --- the network --- */

/* why GitHub can't be used for `needed` requests now, or 0 */
static int net_check(int needed)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq ifr = {0};
	int up = 0;
	if (s >= 0) {
		snprintf(ifr.ifr_name, IFNAMSIZ, "wlan0");
		if (ioctl(s, SIOCGIFADDR, &ifr) == 0)
			up = 1;
		else {
			snprintf(ifr.ifr_name, IFNAMSIZ, "eth0");
			if (ioctl(s, SIOCGIFADDR, &ifr) == 0 || getenv("PM_SD"))
				up = 1;
		}
		close(s);
	} else if (getenv("PM_SD")) {
		up = 1;
	}
	if (!up) {
		snprintf(pm_error, sizeof pm_error, "Wi-Fi is off or not connected.");
		return -1;
	}
	const char *ns[] = {"timeout", "-t", "5", "nslookup", "api.github.com", NULL};
	if (run(ns, NULL) != 0 && run(ns, NULL) != 0) {
		snprintf(pm_error, sizeof pm_error, "Wi-Fi is connected, but there's no internet (github.com can't be found).");
		return -1;
	}
	char rl[400];
	snprintf(rl, sizeof rl, "%s/rate_limit.json", WORK);
	if (fetch("https://api.github.com/rate_limit", rl, 0)) {
		snprintf(pm_error, sizeof pm_error, "The internet works, but GitHub doesn't answer. Try again later.");
		return -1;
	}
	char *text = read_file(rl, NULL);
	cJSON *root = cJSON_Parse(text ? text : "");
	free(text);
	const cJSON *core = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "resources"), "core");
	const cJSON *rem = cJSON_GetObjectItemCaseSensitive(core, "remaining");
	const cJSON *reset = cJSON_GetObjectItemCaseSensitive(core, "reset");
	int left = cJSON_IsNumber(rem) ? rem->valueint : needed;
	long at = cJSON_IsNumber(reset) ? (long)reset->valuedouble : 0;
	cJSON_Delete(root);
	if (left < needed) {
		snprintf(pm_error, sizeof pm_error,
			"GitHub's limit of 60 checks an hour is used up (%d left, %d needed). Try again in %ld min.",
			left, needed, (at - time(NULL)) / 60 + 1);
		return -1;
	}
	return 0;
}

/* the latest release of each repo, fetched once per check */
typedef struct {
	char repo[128];
	cJSON *json;
} Release;
static Release rels[64];
static int nrels;

static Release *release_of(const char *repo)
{
	for (int i = 0; i < nrels; i++)
		if (!strcmp(rels[i].repo, repo))
			return &rels[i];
	return NULL;
}

static void rel_path(const char *repo, char *out, size_t n)
{
	snprintf(out, n, "%s/release_%s.json", WORK, repo);
	for (char *c = out + strlen(WORK) + 1; *c; c++)
		if (*c == '/')
			*c = '_';
}

/* -1 with pm_error when the SD card can't take `need` bytes and 8 MB to spare; nothing was
 * written yet when this is asked */
static int no_space(const char *who, long long need)
{
	long long have = free_bytes(SD), mb = 1024 * 1024;
	if (have < 0 || have >= need + 8 * mb)
		return 0;
	snprintf(pm_error, sizeof pm_error, "%s: not enough free space on the SD card (needs %lld MB, %lld MB free): nothing was changed",
		who, (need + 8 * mb + mb - 1) / mb, have / mb);
	say("ERROR %s", pm_error);
	return -1;
}

static void set_stage(const char *label)
{
	if (on_stage)
		on_stage(label);
}

/* fetches the repos' latest releases, all at once (one wget each), then retries the failed;
 * with `names` (a port of each repo), shows which one it is waiting for */
static void fetch_releases(char **repos, const char **names, int n)
{
	if (n > 64)
		n = 64;
	char f[64][512], part[64][600], url[64][300];
	const char *tok[64];
	for (int i = 0; i < n; i++) {
		rel_path(repos[i], f[i], sizeof f[i]);
		snprintf(part[i], sizeof part[i], "%s.part", f[i]);
		snprintf(url[i], sizeof url[i], API "%s/releases/latest", repos[i]);
		del_file(f[i]);
		del_file(part[i]);
		tok[i] = token_for_repo(repos[i]);
	}

	/* One curl per token, all its repos over one connection: a TLS handshake takes ~2 s of the
	 * Miyoo's CPU, so 8 parallel wgets took 6.5 s and one curl 2.4 s (owner's Mini Plus, 2026-10-19).
	 * curl writes the files in order, so the newest one is the repo being fetched. */
	const char *ca = wget_ca_cert();
	int group[64], pids[64], ngroups = 0;
	for (int i = 0; i < n; i++) {
		group[i] = -1;
		for (int j = 0; j < i && group[i] < 0; j++)
			if (tok[j] == tok[i])
				group[i] = group[j];
		if (group[i] >= 0)
			continue;
		group[i] = ngroups;
		const char *argv[16 + 2 * 64];
		int argc = 0;
		char auth_hdr[300];
		argv[argc++] = curl_bin();
		argv[argc++] = "-s";
		argv[argc++] = "-L";
		argv[argc++] = "--connect-timeout";
		argv[argc++] = "10";
		argv[argc++] = "-m";
		argv[argc++] = "15";
		if (ca) {
			argv[argc++] = "--cacert";
			argv[argc++] = ca;
		}
		if (tok[i]) {
			snprintf(auth_hdr, sizeof auth_hdr, "Authorization: Bearer %s", tok[i]);
			argv[argc++] = "-H";
			argv[argc++] = auth_hdr;
		}
		for (int k = i; k < n; k++)
			if (tok[k] == tok[i]) {
				argv[argc++] = "-o";
				argv[argc++] = part[k];
				argv[argc++] = url[k];
			}
		argv[argc] = NULL;
		pids[ngroups++] = run_bg(argv, NULL);
	}
	int shown = -1, shown_done = -1;
	for (int left = ngroups; left > 0;) {
		left = 0;
		for (int g = 0; g < ngroups; g++)
			if (pids[g] > 0) {
				int st;
				if (waitpid(pids[g], &st, WNOHANG) == 0)
					left++;
				else
					pids[g] = -1;
			}
		if (names) {
			/* a repo is done when its curl ended or the next file of the same curl began */
			int done = 0, now = -1;
			for (int i = 0; i < n; i++) {
				int later = 0;
				for (int k = i + 1; k < n && !later; k++)
					later = group[k] == group[i] && file_exists(part[k]);
				if (pids[group[i]] < 0 || later)
					done++;
				else if (now < 0)
					now = i;
			}
			if (now >= 0 && now != shown) {
				char label[80];
				snprintf(label, sizeof label, "Checking %s", names[now]);
				set_stage(label);
				shown = now;
				shown_done = -1; /* a new label clears the bar */
			}
			if (on_check_progress && done != shown_done)
				on_check_progress(shown_done = done, n);
		}
		if (left)
			usleep(100000);
	}
	for (int i = 0; i < n; i++) {
		/* what didn't come: once more on its own (wget or curl, as fetch_auth chooses) */
		if (!(file_size(part[i]) > 0 && rename(part[i], f[i]) == 0))
			fetch_auth(url[i], f[i], 0, tok[i], NULL);
		del_file(part[i]);
		char *text = read_file(f[i], NULL);
		cJSON *j = text ? cJSON_Parse(text) : NULL;
		free(text);
		if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "tag_name"))) {
			cJSON_Delete(j);
			j = NULL;
			/* If releases/latest failed (e.g. repo only has prereleases), try releases?per_page=5 */
			char url_all[300];
			snprintf(url_all, sizeof url_all, API "%s/releases?per_page=5", repos[i]);
			if (fetch_auth(url_all, f[i], 0, tok[i], NULL) == 0) {
				text = read_file(f[i], NULL);
				cJSON *arr = text ? cJSON_Parse(text) : NULL;
				free(text);
				if (cJSON_IsArray(arr)) {
					const cJSON *elem;
					cJSON_ArrayForEach(elem, arr) {
						const cJSON *draft = cJSON_GetObjectItemCaseSensitive(elem, "draft");
						const cJSON *tag = cJSON_GetObjectItemCaseSensitive(elem, "tag_name");
						if ((!draft || !cJSON_IsTrue(draft)) && cJSON_IsString(tag)) {
							j = cJSON_Duplicate(elem, 1);
							break;
						}
					}
				}
				cJSON_Delete(arr);
			}
		}
		Release *r = release_of(repos[i]);
		if (!r && nrels < 64) {
			r = &rels[nrels++];
			snprintf(r->repo, sizeof r->repo, "%s", repos[i]);
		}
		if (r) {
			cJSON_Delete(r->json);
			r->json = j;
		}
		else
			cJSON_Delete(j);
	}
}

/* entries in `newf` that `oldf` doesn't have, that differ, and that are gone (by id) */
static void catalog_changes(const char *oldf, const char *newf, int *added, int *changed, int *removed)
{
	cJSON *o = read_json(oldf), *n = read_json(newf);
	const cJSON *ol = cJSON_GetObjectItemCaseSensitive(o, "ports"), *nl = cJSON_GetObjectItemCaseSensitive(n, "ports");
	const cJSON *a, *b;
	*added = *changed = *removed = 0;
	cJSON_ArrayForEach(a, nl) {
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(a, "id"), *same = NULL;
		cJSON_ArrayForEach(b, ol)
			if (cJSON_IsString(id) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(b, "id")) &&
				!strcmp(id->valuestring, cJSON_GetObjectItemCaseSensitive(b, "id")->valuestring))
				same = b;
		if (!same)
			(*added)++;
		else if (!cJSON_Compare(a, same, 1))
			(*changed)++;
	}
	int on = cJSON_GetArraySize(ol), nn = cJSON_GetArraySize(nl);
	*removed = on - (nn - *added);
	if (*removed < 0)
		*removed = 0;
	cJSON_Delete(o);
	cJSON_Delete(n);
}

/* one line on the screen: "Catalog:" with its bar, then what changed */
static int refresh_catalog(void)
{
	char f[400];
	snprintf(f, sizeof f, "%s/catalog.json", WORK);
	set_stage("Catalog:");
	long guess = file_size(CAT);
	if (fetch(CAT_URL, f, guess > 0 ? guess : 32 * 1024)) {
		set_stage("");
		say("Catalog: can't download it, the saved one stays");
		return -1;
	}
	set_stage("");
	if (same_file(f, CAT)) {
		say("Catalog: up to date");
		return 0;
	}
	int added, changed, removed;
	catalog_changes(CAT, f, &added, &changed, &removed);
	/* only a catalog that loads replaces the one on the card */
	Port *old = ports;
	int oldn = nports;
	if (load_catalog(f)) {
		ports = old;
		nports = oldn;
		say("Catalog: the new one is broken, the saved one stays");
		return -1;
	}
	copy_file(f, CAT);
	char s[120] = "";
	int l = 0;
	if (added)
		l += snprintf(s + l, sizeof s - l, "%s%d new", l ? ", " : "", added);
	if (changed)
		l += snprintf(s + l, sizeof s - l, "%s%d updated", l ? ", " : "", changed);
	if (removed)
		l += snprintf(s + l, sizeof s - l, "%s%d removed", l ? ", " : "", removed);
	say("Catalog: %s", l ? s : "up to date");
	return 0;
}

static const cJSON *find_asset(const Port *p, const cJSON *rel_json)
{
	if (!rel_json || !p->asset || !*p->asset)
		return NULL;
	regex_t re;
	if (regcomp(&re, p->asset, REG_EXTENDED | REG_NOSUB))
		return NULL;
	const cJSON *a, *asset = NULL;
	cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(rel_json, "assets")) {
		const cJSON *nm = cJSON_GetObjectItemCaseSensitive(a, "name");
		if (cJSON_IsString(nm) && !regexec(&re, nm->valuestring, 0, NULL, 0)) {
			asset = a;
			break;
		}
	}
	regfree(&re);
	return asset;
}

int pm_check(const Port *only)
{
	pm_error[0] = 0;
	/* a new catalog reallocates ports: only for the whole check, `only` must stay valid */
	if (!only) {
		refresh_catalog();
		say("Checking installed ports...");
	}
	pm_scan();
	char *repos[64];
	const char *names[64];
	int n = 0;
	if (only) {
		if (*only->asset && *only->repo) {
			names[n] = only->name;
			repos[n++] = only->repo;
		}
	} else {
		for (int i = 0; i < nports; i++) {
			Port *p = &ports[i];
			if (!p->installed || !*p->asset || !*p->repo)
				continue;
			int seen = 0;
			for (int j = 0; j < n; j++)
				seen |= !strcmp(repos[j], p->repo);
			if (!seen && n < 64) {
				names[n] = p->name;
				repos[n++] = p->repo;
			}
		}
	}
	if (net_check(n)) {
		say("NONET %s", pm_error);
		return -1;
	}
	fetch_releases(repos, names, n);
	if (on_check_progress)
		on_check_progress(n, n);

	int failed = 0;
	for (int i = 0; i < nports; i++) {

		Port *p = &ports[i];
		if (only ? (p != only) : (!p->installed || !*p->asset || !*p->repo))
			continue;
		Release *r = release_of(p->repo);
		const cJSON *tag = (r && r->json) ? cJSON_GetObjectItemCaseSensitive(r->json, "tag_name") : NULL;
		const cJSON *ast = (r && r->json) ? find_asset(p, r->json) : NULL;
		p->asset_size = 0;
		p->asset_name[0] = 0;
		if (ast && cJSON_IsString(tag)) {
			p->checked = 1;
			snprintf(p->latest, sizeof p->latest, "%s", tag->valuestring);
			const cJSON *sz = cJSON_GetObjectItemCaseSensitive(ast, "size");
			const cJSON *nm = cJSON_GetObjectItemCaseSensitive(ast, "name");
			p->asset_size = cJSON_IsNumber(sz) ? (long)sz->valuedouble : 0;
			if (cJSON_IsString(nm))
				snprintf(p->asset_name, sizeof p->asset_name, "%s", nm->valuestring);
		} else if (r && r->json) {
			/* Repo checked, but this release has no matching asset for this port */
			p->checked = 1;
			p->latest[0] = 0;
		} else {
			say("ERROR %s: no release info", p->name);
			failed++;
		}
	}
	pm_scan();
	if (failed) {
		if (!pm_error[0])
			snprintf(pm_error, sizeof pm_error, "No release info for %d port(s): the connection broke off. Try again.", failed);
		return -1;
	}
	return 0;
}

const char *pm_release_notes(const Port *p)
{
	if (!p || !p->repo)
		return NULL;
	Release *r = release_of(p->repo);
	if (!r || !r->json)
		return NULL;
	const cJSON *b = cJSON_GetObjectItemCaseSensitive(r->json, "body");
	return (cJSON_IsString(b) && *b->valuestring) ? b->valuestring : NULL;
}

/* --- installing a release --- */

enum { M_GAME, M_IMG, M_SHORTCUT, M_TEMPLATE };
typedef struct {
	int kind;
	char src[700], dst[700]; /* src absolute, dst relative to the card */
} MapEntry;

typedef struct {
	const Port *p;
	const char *stage, *base;
	MapEntry *e;
	int n, cap;
} Map;

static void map_add(Map *m, int kind, const char *src, const char *dst)
{
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 64;
		m->e = realloc(m->e, m->cap * sizeof *m->e);
	}
	MapEntry *e = &m->e[m->n++];
	e->kind = kind;
	snprintf(e->src, sizeof e->src, "%s", src);
	snprintf(e->dst, sizeof e->dst, "%s", dst);
}

static void map_cb(const char *rel, void *ctx)
{
	Map *m = ctx;
	const Port *p = m->p;
	char src[800], pat[300], pre[300];
	snprintf(src, sizeof src, "%s/%s", m->stage, rel);
	for (int i = 0; i < p->nskip; i++) {
		snprintf(pat, sizeof pat, "%s*", p->skip[i]);
		if (!fnmatch(pat, rel, 0))
			return;
	}
	int app = !strcmp(p->layout, "app-to-ports");
	snprintf(pre, sizeof pre, "App/%s/", m->base);
	if (app && !strncmp(rel, pre, strlen(pre))) {
		if (strcmp(rel + strlen(pre), "config.json")) {
			char dst[700];
			snprintf(dst, sizeof dst, "%s/%s", p->dir, rel + strlen(pre));
			map_add(m, M_GAME, src, dst);
		}
	}
	else if (!strncmp(rel, "Roms/PORTS/Imgs/", 16)) {
		/* a second port of one game: its picture goes by the entry's name, like its shortcut */
		char dst[700];
		if (app && *p->app && ends_with(rel, ".png"))
			snprintf(dst, sizeof dst, "Roms/PORTS/Imgs/%s.png", p->name);
		else
			snprintf(dst, sizeof dst, "%s", rel);
		map_add(m, M_IMG, src, dst);
	}
	else if (!strncmp(rel, "Roms/PORTS/Shortcuts/", 21) && (strstr(rel, ".port") || ends_with(rel, ".notfound"))) {
		char bn[256];
		snprintf(bn, sizeof bn, "%s", base_name(rel));
		/* a shortcut shipped as .notfound (Onion's "data not found yet") goes in as .port,
		 * unless the release has the .port too; the game page tells what's missing */
		if (ends_with(bn, ".notfound")) {
			char twin[900];
			snprintf(twin, sizeof twin, "%.*s.port", (int)(strlen(src) - 9), src);
			if (file_exists(twin))
				return;
			snprintf(bn + strlen(bn) - 9, sizeof bn - (strlen(bn) - 9), ".port");
		}
		if (app)
			map_add(m, M_TEMPLATE, src, "");
		else {
			Find f = {bn, NULL, ""};
			walk_files(SC, find_cb, &f);
			if (!*f.found)
				snprintf(f.found, sizeof f.found, "Roms/PORTS/Shortcuts/%s/%s", p->category, bn);
			map_add(m, M_SHORTCUT, src, f.found);
		}
	}
	else if (!app && !strncmp(rel, "Roms/PORTS/Games/", 17)) {
		char expected_prefix[700];
		snprintf(expected_prefix, sizeof expected_prefix, "%s/", p->dir);
		if (!strncmp(rel, expected_prefix, strlen(expected_prefix)))
			map_add(m, M_GAME, src, rel);
		else
			say("%s: skipped foreign release file %s", p->name, rel);
	}
}

/* our shortcut from the release's: its settings (GameName, FullResolution, ...) without its
 * App/ paths, then our GameDir and launch */
static int shortcut_from(const Port *p, const char *tpl, const char *out)
{
	char *t = read_file(tpl, NULL);
	if (!t)
		return -1;
	char text[4096], g[600];
	snprintf(g, sizeof g, "%s", gdir(p));
	char name[256];
	snprintf(name, sizeof name, "%s", *p->app ? p->name : base_name(tpl));
	char *dot = strstr(name, ".port");
	if (dot)
		*dot = 0;
	int n = snprintf(text, sizeof text, "#!/bin/sh\n# %s: runs %s/%s. Written by Port Manager (portmanager-miyoo).\n",
		name, p->dir, p->sc_run);
	if (n < 0 || n >= (int)sizeof text) {
		free(t);
		return -1;
	}
	for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
		size_t k = 0;
		while ((line[k] >= 'A' && line[k] <= 'Z') || (line[k] >= 'a' && line[k] <= 'z') || line[k] == '_' ||
			(k && line[k] >= '0' && line[k] <= '9'))
			k++;
		if (!k || line[k] != '=' || strstr(line, "App/") || !strncmp(line, "APP=", 4) ||
			!strncmp(line, "GameDir=", 8) || !strncmp(line, "GameDataFile=", 13) ||
			(*p->app && !strncmp(line, "GameName=", 9)))
			continue;
		/* Reject control characters or backticks in lines copied from template */
		if (strpbrk(line, "`$;|"))
			continue;
		int w = snprintf(text + n, sizeof text - n, "%s\n", line);
		if (w < 0 || w >= (int)(sizeof text - n)) {
			free(t);
			return -1;
		}
		n += w;
	}
	free(t);
	/* a second port of one game is listed under the entry's name */
	if (*p->app && !strpbrk(p->name, "\"`$\\")) {
		int w = snprintf(text + n, sizeof text - n, "GameName=\"%s\"\n", p->name);
		if (w < 0 || w >= (int)(sizeof text - n))
			return -1;
		n += w;
	}
	int w = snprintf(text + n, sizeof text - n, "GameDir=\"%s\"\nGameDataFile=\"%s\"\ncd \"%s\" || exit 1\nexec sh \"%s/%s\"\n",
		base_name(p->dir), p->sc_gdf, g, g, p->sc_run);
	if (w < 0 || w >= (int)(sizeof text - n))
		return -1;
	n += w;
	return write_file(out, text, n);
}

static int unpack(const char *file, const char *to)
{
	mkdirs(to);
	if (ends_with(file, ".zip")) {
		mz_zip_archive z;
		memset(&z, 0, sizeof z);
		if (!mz_zip_reader_init_file(&z, file, 0))
			return -1;
		int ok = 1;
		mz_uint total_zip = mz_zip_reader_get_num_files(&z);
		mz_uint extracted = 0;
		for (mz_uint i = 0; i < total_zip; i++) {
			mz_zip_archive_file_stat st;
			if (!mz_zip_reader_file_stat(&z, i, &st) || mz_zip_reader_is_file_a_directory(&z, i))
				continue;
			if (strstr(st.m_filename, "..") || st.m_filename[0] == '/')
				continue;
			char out[1024];
			snprintf(out, sizeof out, "%s/%s", to, st.m_filename);
			mkdirs_for(out);
			if (!mz_zip_reader_extract_to_file(&z, i, out, 0))
				ok = 0;
			extracted++;
			if (on_extract_progress)
				on_extract_progress(extracted, total_zip);
		}
		mz_zip_reader_end(&z);
		return ok ? 0 : -1;
	}
	char o[1100];
	snprintf(o, sizeof o, "-o%s", to);
	const char *x7[] = {"7z", "x", "-y", o, file, NULL};
	const char *tgz[] = {"tar", "-xzf", file, "-C", to, NULL};
	const char *tar[] = {"tar", "-xf", file, "-C", to, NULL};
	if (ends_with(file, ".7z"))
		return run(x7, NULL) ? -1 : 0;
	if (ends_with(file, ".tar.gz") || ends_with(file, ".tgz"))
		return run(tgz, NULL) ? -1 : 0;
	if (ends_with(file, ".tar"))
		return run(tar, NULL) ? -1 : 0;
	return -1;
}

static int manifest_has(const char *manifest, const char *hash, const char *dst)
{
	char line[900];
	snprintf(line, sizeof line, "%s %s\n", hash, dst);
	return manifest && strstr(manifest, line) != NULL;
}

static int fail(const char *fmt, const char *a)
{
	snprintf(pm_error, sizeof pm_error, fmt, a);
	say("ERROR %s", pm_error);
	return -1;
}

/* before a new folder is moved in whole: what the catalog doesn't install (skip, App's
 * config.json) leaves it first */
typedef struct {
	const Map *m;
	const char *replace, *root;
} Prune;

static void prune_cb(const char *rel, void *ctx)
{
	Prune *pr = ctx;
	char src[900];
	snprintf(src, sizeof src, "%s/%s", pr->root, rel);
	for (int i = 0; i < pr->m->n; i++)
		if (pr->replace[i] && pr->m->e[i].kind == M_GAME && !strcmp(pr->m->e[i].src, src))
			return;
	del_file(src);
}

static int apply_release(Port *p, const char *tag, const char *aname, const char *url_str,
	const char *api_url_str, const char *digest_str, long total_bytes)
{
	pm_error[0] = 0;
	shortcuts_changed = 0;
	int fresh = !p->installed;
	char zip[600], stage[400], g[600], bk_stage[700];
	snprintf(zip, sizeof zip, "%s/%s", WORK, aname);
	snprintf(stage, sizeof stage, "%s/stage", WORK);
	snprintf(g, sizeof g, "%s", gdir(p));
	snprintf(bk_stage, sizeof bk_stage, "%s/bk_stage", WORK);
	remove_tree(bk_stage);

	const char *base = app_name(p);
	char basebuf[256];
	snprintf(basebuf, sizeof basebuf, "%s", base);
	base = basebuf;

	/* the zip and its unpacked files (moved into place, not copied), with room to spare */
	if (no_space(p->name, total_bytes > 0 ? 3LL * total_bytes : 0))
		return -1;
	set_stage("1/3 Downloading");
	if (total_bytes >= 1024 * 1024)
		say_log("%s: %s -> %s, downloading %s (%.1f MB)", p->name, *p->version ? p->version : "?", tag, aname,
			(double)total_bytes / (1024.0 * 1024.0));
	else if (total_bytes > 0)
		say_log("%s: %s -> %s, downloading %s (%ld KB)", p->name, *p->version ? p->version : "?", tag, aname,
			(total_bytes + 1023) / 1024);
	else
		say_log("%s: %s -> %s, downloading %s", p->name, *p->version ? p->version : "?", tag, aname);
	const char *tok = token_for_repo(p->repo);
	int dl_ok = 0;
	if (tok) {
		const char *target_url = (api_url_str && *api_url_str) ? api_url_str : ((url_str && *url_str) ? url_str : NULL);
		if (target_url && fetch_auth(target_url, zip, total_bytes, tok, "Accept: application/octet-stream") == 0)
			dl_ok = 1;
	}
	if (!dl_ok) {
		if (url_str && *url_str && fetch(url_str, zip, total_bytes) == 0)
			dl_ok = 1;
	}
	if (!dl_ok)
		return fail("%s: download failed", p->name);
	if (digest_str && !strncmp(digest_str, "sha256:", 7)) {
		char h[65];
		if (sha256_file(zip, h) || strcmp(h, digest_str + 7)) {
			del_file(zip);
			return fail("%s: the download's sha256 is wrong", p->name);
		}
	}
	else if (total_bytes > 0 && file_size(zip) != total_bytes) {
		del_file(zip);
		return fail("%s: the download's size is wrong", p->name);
	}
	remove_tree(stage);
	set_stage("2/3 Unpacking");
	say_log("%s: unpacking...", p->name);
	if (on_extract_progress)
		on_extract_progress(0, 1);
	if (unpack(zip, stage)) {
		del_file(zip);
		return fail("%s: can't unpack the download", p->name);
	}
	if (on_extract_progress)
		on_extract_progress(1, 1);
	del_file(zip);

	/* an older install in App/ (miyoo64's own layout): moved to the Ports folder first */
	if (legacy_app(p)) {
		char app[600], cfg[700], bcfg[900];
		snprintf(app, sizeof app, "%s/App/%s", SD, base);
		if (is_dir(g))
			return fail("%s: it's both in App/ and in the Ports folder: sort it out by hand", p->name);
		if (rename(app, g))
			return fail("%s: can't move it out of App/", p->name);
		snprintf(cfg, sizeof cfg, "%s/config.json", g);
		snprintf(bcfg, sizeof bcfg, "%s/App/%s/config.json", bk_stage, base);
		if (file_exists(cfg) && !mkdirs_for(bcfg))
			rename(cfg, bcfg);
		say("%s: moved App/%s -> %s", p->name, base, p->dir);
	}

	Map m = {p, stage, base, NULL, 0, 0};
	walk_files(stage, map_cb, &m);
	for (int i = 0; i < m.n; i++)
		if (m.e[i].kind == M_TEMPLATE) {
			char gen[400];
			snprintf(gen, sizeof gen, "%s/shortcut.port", WORK);
			char tpl[700];
			snprintf(tpl, sizeof tpl, "%s", m.e[i].src);
			if (shortcut_from(p, tpl, gen) == 0) {
				/* ours is found by its GameDir; with `app` it is named after the entry, so it
				 * doesn't take the other port's shortcut */
				Find f = {NULL, base_name(p->dir), ""};
				walk_files(SC, find_cb, &f);
				if (!*f.found && *p->app)
					snprintf(f.found, sizeof f.found, "Roms/PORTS/Shortcuts/%s/%s.port", p->category, p->name);
				else if (!*f.found)
					snprintf(f.found, sizeof f.found, "Roms/PORTS/Shortcuts/%s/%s", p->category, base_name(tpl));
				map_add(&m, M_SHORTCUT, gen, f.found);
			}
			break;
		}

	char manf[700];
	snprintf(manf, sizeof manf, "%s/.pm_manifest", g);
	char *oldman = read_file(manf, NULL);
	size_t mcap = 4096, mlen = 0;
	char *newman = malloc(mcap);
	newman[0] = 0;
	/* first what changes: files already the same, and the player's own edits, stay as they are */
	char *replace = calloc(m.n + 1, 1);
	int total_to_copy = 0;
	for (int i = 0; i < m.n; i++) {
		MapEntry *e = &m.e[i];
		if (e->kind == M_TEMPLATE)
			continue;
		char d[1000], h[65];
		snprintf(d, sizeof d, "%s/%s", SD, e->dst);
		if (file_exists(d)) {
			if (same_file(e->src, d)) {
				if (sha256_file(e->src, h))
					h[0] = 0;
				if (mlen + strlen(e->dst) + 80 > mcap)
					newman = realloc(newman, mcap *= 2);
				mlen += sprintf(newman + mlen, "%s %s\n", h, e->dst);
				continue;
			}
			if (e->kind != M_GAME) {
				char oh[65];
				if (sha256_file(d, oh) == 0) {
					int ours = manifest_has(oldman, oh, e->dst);
					if (e->kind == M_SHORTCUT) {
						char *t = read_file(d, NULL);
						ours |= t && (strstr(t, "Written by update_miyoo64.sh") || strstr(t, "Written by Port Manager") || strstr(t, "Written by Port manager"));
						free(t);
					}
					if (!ours) {
						say("%s: kept your %s", p->name, e->dst);
						continue;
					}
				}
			}
		}
		replace[i] = 1;
		total_to_copy++;
	}
	/* then the backup, before anything is written: the port's backup folders, version and the files to be replaced
	 * (nothing to keep on a fresh install) */
	int anything_old = is_dir(bk_stage);
	{
		char old_vf[700];
		snprintf(old_vf, sizeof old_vf, "%s/.pm_version", g);
		anything_old |= file_exists(old_vf);
	}
	for (int b = 0; b < p->nbackup && !anything_old; b++) {
		char from[800];
		snprintf(from, sizeof from, "%s/%s", g, p->backup[b]);
		anything_old = is_dir(from) || file_exists(from);
	}
	for (int i = 0; i < m.n && !anything_old; i++) {
		char d[1000];
		snprintf(d, sizeof d, "%s/%s", SD, m.e[i].dst);
		anything_old = replace[i] && file_exists(d);
	}
	/* now exactly: the backup twice (its folder, then the tar); the new files are on the card
	 * already, unpacked in the work folder, and only moved */
	{
		long long need = 0;
		for (int i = 0; i < m.n; i++)
			if (replace[i]) {
				char d[1000];
				snprintf(d, sizeof d, "%s/%s", SD, m.e[i].dst);
				need += anything_old ? 2 * tree_bytes(d) : 0;
			}
		for (int b = 0; b < p->nbackup && anything_old; b++) {
			char from[800];
			snprintf(from, sizeof from, "%s/%s", g, p->backup[b]);
			need += 2 * tree_bytes(from);
		}
		if (no_space(p->name, need)) {
			free(replace);
			free(oldman);
			free(newman);
			free(m.e);
			remove_tree(stage);
			remove_tree(bk_stage);
			return -1;
		}
	}
	if (is_dir(bk_stage) || (total_to_copy > 0 && anything_old)) {
		set_stage("Backup");
		say_log("%s: backing up...", p->name);
		mkdirs(bk_stage);
		for (int v = 0; v < 2; v++) {
			const char *vname = v ? ".miyoo64_version" : ".pm_version";
			char old_vf[700], bk_vf[1100];
			snprintf(old_vf, sizeof old_vf, "%s/%s", g, vname);
			snprintf(bk_vf, sizeof bk_vf, "%s/%s/%s", bk_stage, p->dir, vname);
			if (file_exists(old_vf))
				copy_file(old_vf, bk_vf);
		}
		for (int b = 0; b < p->nbackup; b++) {
			char from[800], to[1100];
			snprintf(from, sizeof from, "%s/%s", g, p->backup[b]);
			snprintf(to, sizeof to, "%s/%s/%s", bk_stage, p->dir, p->backup[b]);
			if (to[strlen(to) - 1] == '/')
				to[strlen(to) - 1] = 0;
			if (is_dir(from) && copy_tree(from, to) == 0)
				say_log("%s: saved %s/%s to the backup", p->name, p->dir, p->backup[b]);
		}
		for (int i = 0; i < m.n; i++) {
			char d[1000], b[1100];
			snprintf(d, sizeof d, "%s/%s", SD, m.e[i].dst);
			snprintf(b, sizeof b, "%s/%s", bk_stage, m.e[i].dst);
			if (replace[i] && file_exists(d) && copy_file(d, b))
				say("%s: warning: could not back up %s before overwrite", p->name, m.e[i].dst);
		}
		char bk_tar[800];
		snprintf(bk_tar, sizeof bk_tar, "%s/Backups/PortManager/%s-%s-before-%s.tar", SD, STAMP, p->id, tag);
		mkdirs_for(bk_tar);
		const char *tar_cmd[] = {"tar", "-cf", bk_tar, "-C", bk_stage, ".", NULL};
		int tar_failed = run(tar_cmd, NULL);
		remove_tree(bk_stage);
		if (tar_failed) {
			del_file(bk_tar);
			free(replace);
			free(oldman);
			free(newman);
			free(m.e);
			remove_tree(stage);
			return fail("%s: can't create the backup archive, nothing was changed", p->name);
		}
		say_log("backup: %s", bk_tar + strlen(SD) + 1);
		say_screen("Backup saved to Backups/PortManager");
	}
	if (total_to_copy > 0) {
		set_stage("3/3 Installing");
		say_log("%s: installing files...", p->name);
		if (on_copy_progress)
			on_copy_progress(0, total_to_copy);
	}
	/* a new game folder goes in whole: one rename, not one per file (the card is mounted
	 * dirsync, so every rename costs a directory write: ~30 s for 1250 files) */
	char whole[800] = "";
	if (total_to_copy > 0 && !is_dir(g)) {
		if (!strcmp(p->layout, "app-to-ports"))
			snprintf(whole, sizeof whole, "%s/App/%s", stage, base);
		else
			snprintf(whole, sizeof whole, "%s/%s", stage, p->dir);
		Prune pr = {&m, replace, whole};
		if (!is_dir(whole))
			whole[0] = 0;
		else {
			walk_files(whole, prune_cb, &pr);
			if (!guard_ok(whole) || !guard_ok(g) || mkdirs_for(g) || rename(whole, g))
				whole[0] = 0;
			else
				say_log("%s: moved the new folder in whole", p->name);
		}
	}
	int n = 0, failed = 0;
	size_t wl = strlen(whole);
	for (int i = 0; i < m.n && !failed; i++) {
		MapEntry *e = &m.e[i];
		if (!replace[i])
			continue;
		char d[1000], h[65];
		snprintf(d, sizeof d, "%s/%s", SD, e->dst);
		int in_place = wl && e->kind == M_GAME && !strncmp(e->src, whole, wl) && e->src[wl] == '/';
		if (sha256_file(in_place ? d : e->src, h))
			h[0] = 0;
		if (mlen + strlen(e->dst) + 80 > mcap)
			newman = realloc(newman, mcap *= 2);
		/* the work folder is on the card too: moved, not written a second time */
		if (!in_place && move_file(e->src, d)) {
			fail("%s: can't write a file", p->name);
			failed = 1;
			break;
		}
		if (e->kind == M_GAME)
			chmod(d, 0777);
		if (e->kind == M_SHORTCUT)
			shortcuts_changed = 1;
		mlen += sprintf(newman + mlen, "%s %s\n", h, e->dst);
		if (on_file_installed)
			on_file_installed(e->dst);
		n++;
		if (on_copy_progress)
			on_copy_progress(n, total_to_copy);
	}
	free(replace);
	free(oldman);
	free(m.e);
	remove_tree(stage);
	if (failed) {
		free(newman);
		return -1;
	}
	write_file(manf, newman, mlen);
	free(newman);
	char vf[700];
	snprintf(vf, sizeof vf, "%s/.pm_version", g);
	char v[80];
	int vl = snprintf(v, sizeof v, "%s\n", tag);
	write_file(vf, v, vl);
	snprintf(vf, sizeof vf, "%s/.miyoo64_version", g);
	if (file_exists(vf))
		write_file(vf, v, vl);
	if (shortcuts_changed) {
		/* Onion's cached Ports list, rebuilt when the list opens */
		DIR *d = opendir(SC);
		struct dirent *e;
		while (d && (e = readdir(d)))
			if (strstr(e->d_name, "_cache") && strstr(e->d_name, ".db")) {
				char f[600];
				snprintf(f, sizeof f, "%s/%s", SC, e->d_name);
				del_file(f);
			}
		if (d)
			closedir(d);
	}
	if (fresh)
		say("%s: installed %s, %d file(s)", p->name, tag, n);
	else
		say("%s: %s -> %s, %d file(s) replaced", p->name, *p->version ? p->version : "?", tag, n);
	pm_scan();
	return 0;
}

int pm_update(Port *p)
{
	pm_error[0] = 0;
	if (!*p->asset)
		return fail("%s: no releases yet", p->name);
	Release *r = release_of(p->repo);
	if (!r || !r->json) {
		char *repo[] = {p->repo};
		if (net_check(1))
			return -1;
		fetch_releases(repo, NULL, 1);
		r = release_of(p->repo);
		if (!r || !r->json)
			return fail("%s: no release info (no connection?)", p->name);
	}
	const char *tag = cJSON_GetObjectItemCaseSensitive(r->json, "tag_name")->valuestring;
	const cJSON *asset = find_asset(p, r->json);
	if (!asset)
		return fail("%s: the release has no matching file", p->name);
	const char *aname = cJSON_GetObjectItemCaseSensitive(asset, "name")->valuestring;
	const cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
	const cJSON *api_url = cJSON_GetObjectItemCaseSensitive(asset, "url");
	const cJSON *digest = cJSON_GetObjectItemCaseSensitive(asset, "digest");
	const cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
	long total_bytes = cJSON_IsNumber(size) ? (long)size->valuedouble : 0;
	const char *url_str = (cJSON_IsString(url) && *url->valuestring) ? url->valuestring : NULL;
	const char *api_url_str = (cJSON_IsString(api_url) && *api_url->valuestring) ? api_url->valuestring : NULL;
	const char *digest_str = (cJSON_IsString(digest) && *digest->valuestring) ? digest->valuestring : NULL;

	return apply_release(p, tag, aname, url_str, api_url_str, digest_str, total_bytes);
}

int pm_install_release(Port *p, const RemoteRelease *rel)
{
	pm_error[0] = 0;
	if (!rel)
		return fail("%s: invalid release", p->name);
	return apply_release(p, rel->tag, rel->asset_name, rel->download_url, rel->api_url, rel->digest, rel->asset_size);
}

int pm_list_backups(const Port *p, BackupItem **items, int *n)
{
	*items = NULL;
	*n = 0;
	char bdir[600];
	snprintf(bdir, sizeof bdir, "%s/Backups/PortManager", SD);
	DIR *d = opendir(bdir);
	if (!d)
		return 0;

	int cap = 16, count = 0;
	BackupItem *list = malloc(cap * sizeof *list);

	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		const char *match = strstr(e->d_name, p->id);
		if (!match)
			continue;
		if (match > e->d_name && match[-1] != '-')
			continue;
		size_t idlen = strlen(p->id);
		if (match[idlen] != '-' && match[idlen] != '.' && match[idlen] != '\0')
			continue;

		char full[800];
		snprintf(full, sizeof full, "%s/%s", bdir, e->d_name);

		int is_tar = ends_with(e->d_name, ".tar") || ends_with(e->d_name, ".tar.gz") || ends_with(e->d_name, ".tgz");
		int is_zip = ends_with(e->d_name, ".zip");
		int is_dir_entry = is_dir(full);
		if (!is_tar && !is_zip && !is_dir_entry)
			continue;

		if (count == cap) {
			cap *= 2;
			list = realloc(list, cap * sizeof *list);
		}
		BackupItem *b = &list[count++];
		memset(b, 0, sizeof *b);
		snprintf(b->path, sizeof b->path, "%s", full);
		b->is_archive = is_tar || is_zip;
		b->size = is_dir_entry ? 0 : file_size(full);

		if (strlen(e->d_name) >= 15 && e->d_name[8] == '-') {
			snprintf(b->date, sizeof b->date, "%.4s-%.2s-%.2s %.2s:%.2s",
				e->d_name, e->d_name + 4, e->d_name + 6, e->d_name + 9, e->d_name + 11);
		} else {
			snprintf(b->date, sizeof b->date, "%.63s", e->d_name);
		}

		const char *bef = strstr(e->d_name, "-before-");
		if (bef) {
			snprintf(b->tag, sizeof b->tag, "%s", bef + 8);
			char *dot = strstr(b->tag, ".tar");
			if (dot) *dot = 0;
			dot = strstr(b->tag, ".zip");
			if (dot) *dot = 0;
			dot = strstr(b->tag, ".tgz");
			if (dot) *dot = 0;
		}

		if (b->size >= 1024 * 1024)
			snprintf(b->display, sizeof b->display, "%s  before %s (%.1f MB)", b->date, *b->tag ? b->tag : "?", (double)b->size / (1024.0 * 1024.0));
		else if (b->size > 0)
			snprintf(b->display, sizeof b->display, "%s  before %s (%ld KB)", b->date, *b->tag ? b->tag : "?", (b->size + 1023) / 1024);
		else
			snprintf(b->display, sizeof b->display, "%s  before %s (folder)", b->date, *b->tag ? b->tag : "?");
	}
	closedir(d);

	for (int i = 0; i < count - 1; i++) {
		for (int j = i + 1; j < count; j++) {
			if (strcmp(list[i].path, list[j].path) < 0) {
				BackupItem tmp = list[i];
				list[i] = list[j];
				list[j] = tmp;
			}
		}
	}

	*items = list;
	*n = count;
	return 0;
}

void pm_free_backups(BackupItem *items, int n)
{
	(void)n;
	free(items);
}

static void restore_check_cb(const char *rel, void *ctx)
{
	char full[1200];
	snprintf(full, sizeof full, "%s/%s", SD, rel);
	if (!guard_ok(full))
		((int *)ctx)[0] = 1;
}

int pm_restore_backup(Port *p, const BackupItem *b)
{
	pm_error[0] = 0;
	if (!b || !*b->path)
		return fail("%s: invalid backup", p->name);

	say("%s: restoring from backup...", p->name);
	char rstage[600];
	snprintf(rstage, sizeof rstage, "%s/restore_stage", WORK);
	remove_tree(rstage);
	mkdirs(rstage);

	const char *src_dir = NULL;
	if (b->is_archive) {
		if (unpack(b->path, rstage)) {
			remove_tree(rstage);
			return fail("%s: cannot unpack backup archive", p->name);
		}
		src_dir = rstage;
	} else if (is_dir(b->path)) {
		src_dir = b->path;
	} else {
		remove_tree(rstage);
		return fail("%s: backup not found on card", p->name);
	}

	/* every file of the backup must land in Port Manager's folders, or nothing is restored */
	struct { int bad; } chk = {0};
	walk_files(src_dir, restore_check_cb, &chk);
	if (chk.bad) {
		remove_tree(rstage);
		return fail("%s: the backup has files outside the Ports folders: nothing restored", p->name);
	}
	if (copy_tree(src_dir, SD)) {
		remove_tree(rstage);
		return fail("%s: failed to copy backup files to SD card", p->name);
	}

	char g[600];
	snprintf(g, sizeof g, "%s/%s", SD, p->dir);
	/* the version goes back with the files: none in the backup means it wasn't known then */
	for (int v = 0; v < 2; v++) {
		const char *vname = v ? ".miyoo64_version" : ".pm_version";
		char in_bk[1200], vf[700];
		snprintf(in_bk, sizeof in_bk, "%s/%s/%s", src_dir, p->dir, vname);
		snprintf(vf, sizeof vf, "%s/%s", g, vname);
		if (!file_exists(in_bk))
			del_file(vf);
	}
	remove_tree(rstage);
	if (*p->sc_run) {
		char exe[700];
		snprintf(exe, sizeof exe, "%s/%s", g, p->sc_run);
		chmod(exe, 0777);
	}

	DIR *d = opendir(SC);
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (strstr(e->d_name, "_cache") && strstr(e->d_name, ".db")) {
			char f[600];
			snprintf(f, sizeof f, "%s/%s", SC, e->d_name);
			del_file(f);
		}
	if (d)
		closedir(d);

	pm_scan();
	say("%s: restored from backup (%s)", p->name, *b->tag ? b->tag : b->date);
	return 0;
}

int pm_list_remote_releases(const Port *p, RemoteRelease **items, int *n)
{
	*items = NULL;
	*n = 0;
	pm_error[0] = 0;
	if (!*p->asset || !*p->repo)
		return fail("%s: no repo/releases configured", p->name);

	if (net_check(1))
		return -1;

	char url[400], f[500];
	snprintf(url, sizeof url, API "%s/releases?per_page=30", p->repo);
	rel_path(p->repo, f, sizeof f);
	snprintf(f + strlen(f), sizeof f - strlen(f), "_all.json");
	del_file(f);

	const char *tok = token_for_repo(p->repo);
	if (fetch_auth(url, f, 0, tok, NULL))
		return fail("%s: cannot fetch releases list from GitHub", p->name);

	char *text = read_file(f, NULL);
	del_file(f);
	if (!text)
		return fail("%s: cannot read releases response", p->name);

	cJSON *root = cJSON_Parse(text);
	free(text);
	if (!cJSON_IsArray(root)) {
		cJSON_Delete(root);
		return fail("%s: invalid releases list from GitHub", p->name);
	}

	int cap = 16, count = 0;
	RemoteRelease *list = malloc(cap * sizeof *list);

	const cJSON *elem;
	cJSON_ArrayForEach(elem, root) {
		const cJSON *ast = find_asset(p, elem);
		if (!ast)
			continue;
		const cJSON *tag = cJSON_GetObjectItemCaseSensitive(elem, "tag_name");
		if (!cJSON_IsString(tag) || !*tag->valuestring)
			continue;

		if (count == cap) {
			cap *= 2;
			list = realloc(list, cap * sizeof *list);
		}
		RemoteRelease *rel = &list[count++];
		memset(rel, 0, sizeof *rel);
		snprintf(rel->tag, sizeof rel->tag, "%s", tag->valuestring);

		const cJSON *rname = cJSON_GetObjectItemCaseSensitive(elem, "name");
		if (cJSON_IsString(rname) && *rname->valuestring)
			snprintf(rel->name, sizeof rel->name, "%s", rname->valuestring);
		else
			snprintf(rel->name, sizeof rel->name, "%s", tag->valuestring);

		const cJSON *aname = cJSON_GetObjectItemCaseSensitive(ast, "name");
		if (cJSON_IsString(aname))
			snprintf(rel->asset_name, sizeof rel->asset_name, "%s", aname->valuestring);

		const cJSON *asz = cJSON_GetObjectItemCaseSensitive(ast, "size");
		rel->asset_size = cJSON_IsNumber(asz) ? (long)asz->valuedouble : 0;

		const cJSON *aurl = cJSON_GetObjectItemCaseSensitive(ast, "browser_download_url");
		if (cJSON_IsString(aurl))
			snprintf(rel->download_url, sizeof rel->download_url, "%s", aurl->valuestring);

		const cJSON *api_url = cJSON_GetObjectItemCaseSensitive(ast, "url");
		if (cJSON_IsString(api_url))
			snprintf(rel->api_url, sizeof rel->api_url, "%s", api_url->valuestring);

		const cJSON *digest = cJSON_GetObjectItemCaseSensitive(ast, "digest");
		if (cJSON_IsString(digest))
			snprintf(rel->digest, sizeof rel->digest, "%s", digest->valuestring);

		const cJSON *body = cJSON_GetObjectItemCaseSensitive(elem, "body");
		if (cJSON_IsString(body) && *body->valuestring)
			rel->body = strdup(body->valuestring);
	}
	cJSON_Delete(root);

	if (count == 0) {
		free(list);
		return fail("%s: no releases found with matching game files", p->name);
	}

	*items = list;
	*n = count;
	return 0;
}

void pm_free_remote_releases(RemoteRelease *items, int n)
{
	if (!items)
		return;
	for (int i = 0; i < n; i++)
		free(items[i].body);
	free(items);
}

typedef struct {
	const Port *p;
	char gdir[600];
	char **to_delete;
	int n, cap;
} CleanCtx;

static void clean_cb(const char *rel, void *ctx)
{
	CleanCtx *c = ctx;
	if (!is_data_file(c->p, rel)) {
		if (c->n == c->cap) {
			c->cap = c->cap ? c->cap * 2 : 64;
			c->to_delete = realloc(c->to_delete, c->cap * sizeof *c->to_delete);
		}
		char full[1024];
		snprintf(full, sizeof full, "%s/%s", c->gdir, rel);
		c->to_delete[c->n++] = strdup(full);
	}
}

static void rmdir_empty_subdirs(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char sub[1024];
		snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
		if (is_dir(sub)) {
			rmdir_empty_subdirs(sub);
			rmdir(sub);
		}
	}
	closedir(d);
}

static void clean_port_files(const Port *p)
{
	char g[600];
	snprintf(g, sizeof g, "%s/%s", SD, p->dir);
	char f[700];
	snprintf(f, sizeof f, "%s/.pm_version", g);
	del_file(f);
	snprintf(f, sizeof f, "%s/.miyoo64_version", g);
	del_file(f);
	snprintf(f, sizeof f, "%s/.pm_manifest", g);
	del_file(f);

	CleanCtx ctx = {0};
	ctx.p = p;
	snprintf(ctx.gdir, sizeof ctx.gdir, "%s", g);
	walk_files(g, clean_cb, &ctx);
	if (on_copy_progress)
		on_copy_progress(0, ctx.n);
	for (int i = 0; i < ctx.n; i++) {
		say_log("%s: removing %s", p->name, ctx.to_delete[i] + strlen(g) + 1);
		del_file(ctx.to_delete[i]);
		free(ctx.to_delete[i]);
		if (on_copy_progress)
			on_copy_progress(i + 1, ctx.n);
	}
	say_log("%s: %d program file(s) removed", p->name, ctx.n);
	free(ctx.to_delete);

	rmdir_empty_subdirs(g);
	if (!dir_has_files(g))
		rmdir(g);
}

/* a game folder file by file, for the bar (Frozen Bubble has 3300), then what's left */
typedef struct {
	const char *root;
	long n, total;
} DelCtx;

static void count_cb(const char *rel, void *ctx)
{
	(void)rel;
	((DelCtx *)ctx)->total++;
}

static void del_cb(const char *rel, void *ctx)
{
	DelCtx *d = ctx;
	char f[1100];
	snprintf(f, sizeof f, "%s/%s", d->root, rel);
	del_file(f);
	d->n++;
	if (on_copy_progress)
		on_copy_progress(d->n, d->total);
}

static void remove_game_folder(const char *g)
{
	DelCtx d = {g, 0, 0};
	walk_files(g, count_cb, &d);
	if (on_copy_progress)
		on_copy_progress(0, d.total);
	walk_files(g, del_cb, &d);
	remove_tree(g);
}

int pm_uninstall(Port *p, int keep_data)
{
	pm_error[0] = 0;
	pm_folder_kept = 0;
	if (!p->installed)
		return fail("%s: not installed", p->name);
	say_log("%s: removing port (keep_data=%d)", p->name, keep_data);

	/* 1. Remove shortcut in Roms/PORTS/Shortcuts/ */
	char sc[700];
	find_shortcut(p, sc, sizeof sc);
	if (*sc) {
		char sc_path[1000];
		snprintf(sc_path, sizeof sc_path, "%s/%s", SD, sc);
		say_log("%s: removing shortcut %s", p->name, sc);
		if (del_file(sc_path) == 0)
			say_screen("Shortcut removed from the Ports list.");

		/* Try removing empty category folder, e.g. Roms/PORTS/Shortcuts/RPG */
		char *slash = strrchr(sc_path, '/');
		if (slash) {
			*slash = 0;
			rmdir(sc_path);
		}
	}

	/* 2. Remove box art image in Roms/PORTS/Imgs/ if any */
	char img[700];
	snprintf(img, sizeof img, "%s/Roms/PORTS/Imgs/%s.png", SD, p->name);
	del_file(img);
	if (*p->category) {
		snprintf(img, sizeof img, "%s/Roms/PORTS/Imgs/%s/%s.png", SD, p->category, p->name);
		del_file(img);
	}

	/* 3. Invalidate Onion's cached Ports list */
	DIR *d = opendir(SC);
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (strstr(e->d_name, "_cache") && strstr(e->d_name, ".db")) {
			char cf[600];
			snprintf(cf, sizeof cf, "%s/%s", SC, e->d_name);
			del_file(cf);
		}
	if (d)
		closedir(d);

	/* 4. Remove program files / directory */
	const char *g = gdir(p);
	set_stage(keep_data ? "Removing program files" : "Removing files");
	if (!keep_data) {
		say_log("%s: deleting directory %s", p->name, g);
		remove_game_folder(g);
		if (legacy_app(p)) {
			char app[600];
			snprintf(app, sizeof app, "%s/App/%s", SD, app_name(p));
			remove_tree(app);
		}
	} else {
		say_log("%s: cleaning program files, keeping game data", p->name);
		clean_port_files(p);
		if (legacy_app(p)) {
			char app[600];
			snprintf(app, sizeof app, "%s/App/%s", SD, app_name(p));
			remove_tree(app);
		}
	}

	pm_folder_kept = is_dir(g);
	say_log("%s: removed", p->name);
	pm_scan();
	return 0;
}

static void append_info(char *buf, int size, int *n, const char *fmt, ...)
{
	if (*n >= size - 1)
		return;
	va_list ap;
	va_start(ap, fmt);
	int written = vsnprintf(buf + *n, size - *n, fmt, ap);
	va_end(ap);
	if (written > 0) {
		*n += written;
		if (*n >= size)
			*n = size - 1;
	}
}

/* the port's page at a glance (owner, 2026-10-19: easy to read, the rest under Select) */
void pm_info(const Port *p, char *buf, int size)
{
	int n = 0;
	if (size <= 0)
		return;
	buf[0] = 0;

	/* 1. Where it stands, in one line */
	if (!*p->asset)
		append_info(buf, size, &n, CLR_DIM "No releases yet.\n");
	else if (!p->installed)
		append_info(buf, size, &n, CLR_TEXT "Not installed.%s%s\n",
			p->checked && *p->latest ? " Latest: " : "", p->checked ? p->latest : "");
	else if (!*p->version)
		append_info(buf, size, &n, CLR_WARN "Installed, version unknown.\n");
	else if (p->update)
		append_info(buf, size, &n, CLR_WARN "Update: %s -> %s\n", p->version, p->latest);
	else if (p->checked)
		append_info(buf, size, &n, CLR_GOOD "Installed: %s (up to date)\n", p->version);
	else
		append_info(buf, size, &n, CLR_TEXT "Installed: %s\n", p->version);
	if (!strcmp(p->status, "experimental"))
		append_info(buf, size, &n, CLR_WARN "Experimental: it runs, but rough.\n");
	else if (!strcmp(p->status, "prerelease"))
		append_info(buf, size, &n, CLR_WARN "Test version from its author.\n");

	/* 2. The game files the player adds */
	char need[512] = "";
	if (p->installed || *p->missing)
		snprintf(need, sizeof need, "%s", p->missing);
	else {
		for (int i = 0; i < p->nrequired; i++)
			snprintf(need + strlen(need), sizeof need - strlen(need), "%s%s", *need ? ", " : "", p->required[i]);
		if (p->nrequired_any)
			snprintf(need + strlen(need), sizeof need - strlen(need), "%sone of: %s%s", *need ? ", " : "",
				p->required_any[0], p->nrequired_any > 1 ? ", ..." : "");
		if (*p->rom)
			snprintf(need + strlen(need), sizeof need - strlen(need), "%s%s", *need ? ", " : "", p->rom);
	}
	append_info(buf, size, &n, "\n");
	if (!p->nrequired && !p->nrequired_any && !*p->rom)
		append_info(buf, size, &n, CLR_GOOD "No game files needed.\n");
	else if (p->installed && !*need)
		append_info(buf, size, &n, CLR_GOOD "Game files: OK\n");
	else {
		append_info(buf, size, &n, "%sGame files from your own copy:\n" CLR_TEXT,
			p->installed ? CLR_WARN : CLR_TEXT);
		if (p->rom_name && *p->rom_name)
			append_info(buf, size, &n, "%s\n", p->rom_name);
		if (strpbrk(need, ",;:*"))
			append_info(buf, size, &n, "\n" CLR_DIM "Copy into:\n" CLR_TEXT "%s/\n%s\n", p->dir, need);
		else
			/* a path too long for the screen's 37 columns breaks after the folder, not in the name */
			append_info(buf, size, &n, "\n" CLR_DIM "Save as:\n" CLR_TEXT "%s/%s%s\n", p->dir,
				strlen(p->dir) + 1 + strlen(need) > 37 ? "\n" : "", need);
	}

	append_info(buf, size, &n, "\n");
	if (p->installed && !*need)
		append_info(buf, size, &n, CLR_GOOD "Start: play\n");
	append_info(buf, size, &n, CLR_DIM "Select: more info\n");
}

/* MainUI puts a game it starts at the top of Onion's recent list; a game started from here
 * skips MainUI, so its line (same format) goes on top here, and an older one of it goes.
 * With recents hidden in Onion the list is recentlist-hidden.json. Other lines are copied
 * byte for byte (MainUI leaves odd bytes in some). sc: Roms/PORTS/Shortcuts/... */
static void add_recent(const char *sc)
{
	char list[700], tmp[720];
	snprintf(list, sizeof list, "%s/Roms/recentlist.json", SD);
	if (!file_exists(list)) {
		snprintf(tmp, sizeof tmp, "%s/Roms/recentlist-hidden.json", SD);
		if (file_exists(tmp))
			snprintf(list, sizeof list, "%s", tmp);
	}
	char label[300], path[900], img[900], launch[700];
	snprintf(label, sizeof label, "%s", base_name(sc));
	char *dot = strrchr(label, '.');
	if (dot)
		*dot = 0;
	snprintf(path, sizeof path, "%s/Emu/PORTS/../../%s", SD, sc);
	snprintf(img, sizeof img, "%s/Emu/PORTS/../../Roms/PORTS/Imgs/%s.png", SD, label);
	snprintf(launch, sizeof launch, "%s/Emu/PORTS/launch.sh", SD);
	cJSON *e = cJSON_CreateObject();
	cJSON_AddStringToObject(e, "label", label);
	cJSON_AddStringToObject(e, "rompath", path);
	cJSON_AddStringToObject(e, "imgpath", img);
	cJSON_AddStringToObject(e, "launch", launch);
	cJSON_AddNumberToObject(e, "type", 5);
	char *line = cJSON_PrintUnformatted(e);
	cJSON_Delete(e);
	/* an older line of this game: its rompath ends the same, whatever the card's path */
	cJSON *r = cJSON_CreateString(sc);
	char *q = cJSON_PrintUnformatted(r); /* "Roms/PORTS/Shortcuts/...", escaped as in the list */
	cJSON_Delete(r);
	char same[900];
	snprintf(same, sizeof same, "/%s", q ? q + 1 : sc); /* ends with the closing quote */
	free(q);
	size_t nsame = strlen(same);

	size_t n = 0;
	char *old = read_file(list, &n);
	snprintf(tmp, sizeof tmp, "%s.pm_tmp", list);
	FILE *o = line ? fopen(tmp, "w") : NULL;
	if (!o) {
		say_log("recent list: can't write %s", tmp);
		free(old);
		free(line);
		return;
	}
	fprintf(o, "%s\n", line);
	int dropped = 0;
	for (size_t i = 0; old && i < n;) {
		char *s = old + i, *nl = memchr(s, '\n', n - i);
		size_t len = nl ? (size_t)(nl - s) : n - i;
		char *rp = memmem(s, len, "\"rompath\":", 10);
		if (rp && memmem(rp, len - (rp - s), same, nsame))
			dropped++;
		else if (len)
			fwrite(s, 1, len, o), fputc('\n', o);
		i += len + 1;
	}
	int ok = fclose(o) == 0 && rename(tmp, list) == 0;
	say_log("recent list: %s on top of %s%s", label, list,
		!ok ? " FAILED" : dropped ? ", its older line removed" : "");
	if (!ok)
		remove(tmp);
	free(old);
	free(line);
}

/* Onion's runtime runs .tmp_update/cmd_to_run.sh once Port Manager exits; /tmp/quick_switch
 * makes it skip MainUI (like its startup apps), so the game starts right away and MainUI
 * comes back after it. The command is the one MainUI writes for a port. */
int pm_launch(const Port *p)
{
	char sc[700];
	find_shortcut(p, sc, sizeof sc);
	if (!*sc) {
		snprintf(pm_error, sizeof pm_error, "%s: no shortcut in the Ports list. Install it again (A).", p->name);
		return -1;
	}
	char f[700];
	snprintf(f, sizeof f, "%s/.tmp_update/cmd_to_run.sh", SD);
	FILE *o = fopen(f, "w");
	if (!o) {
		snprintf(pm_error, sizeof pm_error, "Can't start %s (%s).", p->name, strerror(errno));
		return -1;
	}
	fprintf(o, "LD_PRELOAD=%s/miyoo/app/../lib/libpadsp.so \"%s/Emu/PORTS/launch.sh\" \"%s/Emu/PORTS/../../%s\"\n",
		SD, SD, SD, sc);
	fclose(o);
	o = fopen("/tmp/quick_switch", "w");
	if (o)
		fclose(o);
	say_log("--- start %s: %s", p->id, sc);
	add_recent(sc);
	return 0;
}

/* everything about the port, under Select on its page */
void pm_details(const Port *p, char *buf, int size)
{
	int n = 0;
	if (size <= 0)
		return;
	buf[0] = 0;

	/* 1. Installed */
	if (p->installed) {
		if (*p->version)
			append_info(buf, size, &n, CLR_DIM "Installed: " CLR_TEXT "%s\n", p->version);
		else
			append_info(buf, size, &n, CLR_DIM "Installed: " CLR_WARN "version unknown\n");
	} else {
		append_info(buf, size, &n, CLR_DIM "Installed: " CLR_DIM "not installed\n");
	}

	/* 2. Latest; with no asset there's nothing to check, so A does nothing */
	if (!*p->asset) {
		append_info(buf, size, &n, CLR_DIM "Latest:    " CLR_DIM "no releases yet\n");
	} else if (!p->checked) {
		append_info(buf, size, &n, CLR_DIM "Latest:    " CLR_WARN "not checked (A:check)\n");
	} else if (*p->latest) {
		if (p->installed) {
			if (!*p->version)
				append_info(buf, size, &n, CLR_DIM "Latest:    " CLR_WARN "%s (yours unknown)\n", p->latest);
			else if (p->update)
				append_info(buf, size, &n, CLR_DIM "Latest:    " CLR_WARN "%s (update available)\n", p->latest);
			else
				append_info(buf, size, &n, CLR_DIM "Latest:    " CLR_GOOD "%s (up to date)\n", p->latest);
		} else {
			append_info(buf, size, &n, CLR_DIM "Latest:    " CLR_GOOD "%s\n", p->latest);
		}
	} else {
		append_info(buf, size, &n, CLR_DIM "Latest:    " CLR_DIM "no releases yet\n");
	}

	/* 3. Location on card */
	append_info(buf, size, &n, CLR_DIM "Location:  " CLR_TEXT "%s\n", p->dir);

	/* 4. ROM / Game files / Source */
	if (p->rom_name && *p->rom_name)
		append_info(buf, size, &n, CLR_DIM "ROM:       " CLR_TEXT "%s\n", p->rom_name);

	if (*p->missing) {
		append_info(buf, size, &n, CLR_DIM "Files:     " CLR_WARN "missing (%s)\n", p->missing);
	} else if (p->installed || dir_has_files(gdir(p))) {
		append_info(buf, size, &n, CLR_DIM "Files:     " CLR_GOOD "OK\n");
	} else if (p->nrequired == 0 && p->nrequired_any == 0 && (!p->rom || !*p->rom)) {
		append_info(buf, size, &n, CLR_DIM "Files:     " CLR_GOOD "OK (none required)\n");
	} else {
		append_info(buf, size, &n, CLR_DIM "Files:     " CLR_DIM "not found yet\n");
	}

	append_info(buf, size, &n, CLR_DIM "Source:    " CLR_TEXT "github.com/%s\n", p->repo);
	/* the port's author: the repo's owner */
	const char *slash = strchr(p->repo, '/');
	if (slash)
		append_info(buf, size, &n, CLR_DIM "Port by:   " CLR_TEXT "%.*s\n", (int)(slash - p->repo), p->repo);
	if (*p->status)
		append_info(buf, size, &n, CLR_DIM "Status:    %s%s\n",
			strcmp(p->status, "playable") ? CLR_WARN : CLR_GOOD, p->status);

	/* 5. Instructions */
	if (p->get && *p->get)
		append_info(buf, size, &n, "\n" CLR_DIM "Instructions:\n" CLR_TEXT "%s\n", p->get);
}

/* "1.1.10" vs "1.1.9", number by number; <0, 0 or >0 */
static int version_cmp(const char *a, const char *b)
{
	while (*a || *b) {
		long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
		if (x != y)
			return x < y ? -1 : 1;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
		if ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b)))
			return strcmp(a, b);
	}
	return 0;
}

int pm_check_self(void)
{
	pm_error[0] = 0;
	pm_self_update_available = 0;
	pm_self_latest_version[0] = 0;
	pm_self_asset_url[0] = 0;
	pm_self_asset_name[0] = 0;
	pm_self_asset_size = 0;
	if (net_check(1)) {
		say("NONET %s", pm_error);
		return -1;
	}
	char *repos[] = {PM_REPO};
	fetch_releases(repos, NULL, 1);
	Release *r = release_of(PM_REPO);
	const cJSON *tag = (r && r->json) ? cJSON_GetObjectItemCaseSensitive(r->json, "tag_name") : NULL;
	if (!cJSON_IsString(tag) || !*tag->valuestring)
		return 0;
	const char *cur = pm_version();
	const char *cur_v = (*cur == 'v' || *cur == 'V') ? cur + 1 : cur;
	const char *tag_v = (*tag->valuestring == 'v' || *tag->valuestring == 'V') ? tag->valuestring + 1 : tag->valuestring;
	if (version_cmp(tag_v, cur_v) <= 0) /* only a newer one, never back to an older release */
		return 0;
	const cJSON *a;
	cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(r->json, "assets")) {
		const cJSON *nm = cJSON_GetObjectItemCaseSensitive(a, "name");
		const cJSON *url = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
		const cJSON *sz = cJSON_GetObjectItemCaseSensitive(a, "size");
		if (cJSON_IsString(nm) && strstr(nm->valuestring, ".zip") && strstr(nm->valuestring, "PortManager")) {
			pm_self_update_available = 1;
			snprintf(pm_self_latest_version, sizeof pm_self_latest_version, "%s", tag->valuestring);
			if (cJSON_IsString(url))
				snprintf(pm_self_asset_url, sizeof pm_self_asset_url, "%s", url->valuestring);
			snprintf(pm_self_asset_name, sizeof pm_self_asset_name, "%s", nm->valuestring);
			if (cJSON_IsNumber(sz))
				pm_self_asset_size = (long)sz->valuedouble;
			say_log("pm: self-update available %s -> %s (asset %s, %ld bytes)",
				cur, pm_self_latest_version, pm_self_asset_name, pm_self_asset_size);
			break;
		}
	}
	return 0;
}

int pm_self_update(void)
{
	pm_error[0] = 0;
	if (!pm_self_asset_url[0])
		return fail("%s: no release download URL found", "Port Manager");

	/* its backup, the zip, the unpacked files and the copy */
	if (no_space("Port Manager", tree_bytes(HERE) + 3LL * (pm_self_asset_size > 0 ? pm_self_asset_size : 1024 * 1024)))
		return -1;

	/* 1. Archive current Port Manager installation to Backups/PortManager/ */
	char bk_tar[800];
	const char *cur_ver = pm_version();
	snprintf(bk_tar, sizeof bk_tar, "%s/Backups/PortManager/%s-PortManager-before-%s.tar",
		SD, STAMP, cur_ver);
	mkdirs_for(bk_tar);
	say("Port Manager: creating backup %s...", base_name(bk_tar));
	const char *tar_cmd[] = {"tar", "-cf", bk_tar, "-C", HERE, ".", NULL};
	if (run(tar_cmd, NULL) == 0)
		say("backup: %s", bk_tar + strlen(SD) + 1);
	else
		say("Port Manager: warning: could not create backup archive");

	/* 2. Download release archive */
	char zip[600];
	snprintf(zip, sizeof zip, "%s/portmanager_update.zip", WORK);
	del_file(zip);
	say("Port Manager: downloading %s...", pm_self_latest_version);
	if (pm_self_asset_size > 0) {
		if (pm_self_asset_size >= 1024 * 1024)
			say("download size: %.1f MB", (double)pm_self_asset_size / (1024.0 * 1024.0));
		else
			say("download size: %ld KB", (pm_self_asset_size + 1023) / 1024);
	}

	const char *tok = token_for_repo(PM_REPO);
	if (fetch_auth(pm_self_asset_url, zip, pm_self_asset_size, tok, NULL) != 0) {
		del_file(zip);
		return fail("%s: download failed", "Port Manager");
	}

	if (pm_self_asset_size > 0 && file_size(zip) != pm_self_asset_size) {
		del_file(zip);
		return fail("%s: download size mismatch", "Port Manager");
	}

	/* 3. Extract to stage folder */
	char stage[600];
	snprintf(stage, sizeof stage, "%s/pm_self_stage", WORK);
	remove_tree(stage);
	mkdirs(stage);
	say("Port Manager: extracting files...");
	if (on_extract_progress)
		on_extract_progress(0, 1);
	if (unpack(zip, stage) != 0) {
		del_file(zip);
		remove_tree(stage);
		return fail("%s: could not unpack update archive", "Port Manager");
	}
	if (on_extract_progress)
		on_extract_progress(1, 1);
	del_file(zip);


	/* 4. Copy files to SD */
	say("Port Manager: applying update...");
	char stage_app[700], stage_roms[700];
	snprintf(stage_app, sizeof stage_app, "%s/App", stage);
	snprintf(stage_roms, sizeof stage_roms, "%s/Roms", stage);

	/* the release's App/PortManager goes where this Port Manager runs from */
	char stage_pm[800];
	snprintf(stage_pm, sizeof stage_pm, "%s/PortManager", stage_app);
	/* the catalog isn't taken from the zip: the check after the restart downloads the latest
	 * and says what changed (the zip carries it for a first install) */
	char stage_cat[900];
	snprintf(stage_cat, sizeof stage_cat, "%s/catalog.json", is_dir(stage_pm) ? stage_pm : stage);
	del_file(stage_cat);
	int copy_failed = copy_tree(is_dir(stage_pm) ? stage_pm : stage, HERE) != 0;

	if (!copy_failed && is_dir(stage_roms)) {
		char dst_roms[600];
		snprintf(dst_roms, sizeof dst_roms, "%s/Roms", SD);
		copy_failed = copy_tree(stage_roms, dst_roms) != 0;
	}
	if (copy_failed) {
		remove_tree(stage);
		return fail("%s: could not write the update; the backup is in Backups/PortManager", "Port Manager");
	}

	/* 5. Set executable permissions */
	char bin[700], launch[700];
	snprintf(bin, sizeof bin, "%s/portmanager", HERE);
	snprintf(launch, sizeof launch, "%s/launch.sh", HERE);
	chmod(bin, 0777);
	chmod(launch, 0777);

	/* 6. Record version in .pm_version */
	char vf[700];
	snprintf(vf, sizeof vf, "%s/.pm_version", HERE);
	char v[80];
	int vl = snprintf(v, sizeof v, "%s\n", pm_self_latest_version);
	write_file(vf, v, vl);

	/* 7. Clear OnionOS Ports list cache */
	DIR *d = opendir(SC);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (strstr(e->d_name, "_cache") && strstr(e->d_name, ".db")) {
			char f[600];
			snprintf(f, sizeof f, "%s/%s", SC, e->d_name);
			del_file(f);
		}
	}
	if (d)
		closedir(d);

	remove_tree(stage);
	say("Port Manager: updated to %s successfully!", pm_self_latest_version);
	return 0;
}

