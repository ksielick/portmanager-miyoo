#include "pm.h"
#include "gfx.h"

#include <dirent.h>
#include <fnmatch.h>
#include <net/if.h>
#include <netinet/in.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
	return 1;
}

static int load_catalog(const char *path)
{
	char *text = read_file(path, NULL);
	cJSON *root = text ? cJSON_Parse(text) : NULL;
	free(text);
	const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "ports"), *e;
	if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) {
		cJSON_Delete(root);
		return -1;
	}
	ports = calloc(cJSON_GetArraySize(list), sizeof *ports);
	nports = 0;
	cJSON_ArrayForEach(e, list) {
		Port *p = &ports[nports];
		const cJSON *data = cJSON_GetObjectItemCaseSensitive(e, "data");
		const cJSON *rom = cJSON_GetObjectItemCaseSensitive(data, "rom");
		const cJSON *sc = cJSON_GetObjectItemCaseSensitive(e, "shortcut");
		p->id = str(e, "id");
		p->name = str(e, "name");
		p->dir = str(e, "dir");
		p->layout = str(e, "layout");
		p->repo = str(e, "repo");
		p->asset = str(e, "asset");
		p->category = str(e, "category");
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
		if (!validate_port(p)) {
			say("catalog: rejected invalid/unsafe port entry %s", p->id ? p->id : "(unnamed)");
			continue;
		}
		nports++;
	}
	cJSON_Delete(root);
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

static int legacy_app(const Port *p)
{
	char a[600];
	snprintf(a, sizeof a, "%s/App/%s/launch.sh", SD, base_name(p->dir));
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

static int is_data_file(const Port *p, const char *rel)
{
	/* Metadata files are never data */
	if (!strcmp(rel, ".pm_version") || !strcmp(rel, ".miyoo64_version") || !strcmp(rel, ".pm_manifest"))
		return 0;

	/* Scripts, libraries, and binaries are never data */
	if (ends_with(rel, ".sh") || ends_with(rel, ".so") || strstr(rel, ".so."))
		return 0;
	if (!strncmp(rel, "lib/", 4) || !strncmp(rel, "bin/", 4) || !strncmp(rel, "miyoo/", 6))
		return 0;

	char full[1024];
	snprintf(full, sizeof full, "%s/%s", gdir(p), rel);
	if (is_code_file(full))
		return 0;

	/* 1. ROM file */
	if (*p->rom && (!strcasecmp(rel, p->rom) || !strcasecmp(base_name(rel), p->rom)))
		return 1;

	/* 2. Common ROM / CD image / save extensions */
	const char *exts[] = {
		".z64", ".n64", ".v64", ".iso", ".cue", ".chd", ".pbp", ".rom",
		".sav", ".srm", ".eep", ".fla", ".mpk", ".state", ".cfg", ".ini",
		".mp3", ".ogg", ".wav", ".mid", ".midi", ".flac"
	};
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if (ends_with(rel, exts[i]))
			return 1;

	/* 3. Catalog required files */
	for (int i = 0; i < p->nrequired; i++) {
		const char *req = p->required[i];
		size_t reqlen = strlen(req);
		if (reqlen > 0 && req[reqlen - 1] == '/' && !strncasecmp(rel, req, reqlen))
			return 1;
		if (!fnmatch(req, rel, FNM_CASEFOLD) || !fnmatch(req, base_name(rel), FNM_CASEFOLD))
			return 1;
	}

	/* 4. Catalog required_any files */
	for (int i = 0; i < p->nrequired_any; i++) {
		const char *req = p->required_any[i];
		size_t reqlen = strlen(req);
		if (reqlen > 0 && req[reqlen - 1] == '/' && !strncasecmp(rel, req, reqlen))
			return 1;
		if (!fnmatch(req, rel, FNM_CASEFOLD) || !fnmatch(req, base_name(rel), FNM_CASEFOLD))
			return 1;
	}

	/* 5. Catalog backup directories / files (e.g. saves, config) */
	for (int i = 0; i < p->nbackup; i++) {
		const char *bk = p->backup[i];
		size_t bklen = strlen(bk);
		if (bklen > 0 && bk[bklen - 1] == '/' && !strncasecmp(rel, bk, bklen))
			return 1;
		if (!fnmatch(bk, rel, FNM_CASEFOLD) || !fnmatch(bk, base_name(rel), FNM_CASEFOLD))
			return 1;
	}

	/* 6. Catalog built files / folders */
	for (int i = 0; i < p->nbuilt; i++) {
		const char *bk = p->built[i];
		size_t bklen = strlen(bk);
		if (bklen > 0 && bk[bklen - 1] == '/' && !strncasecmp(rel, bk, bklen))
			return 1;
		if (!fnmatch(bk, rel, FNM_CASEFOLD) || !fnmatch(bk, base_name(rel), FNM_CASEFOLD))
			return 1;
	}

	/* 7. Standard save / data folder names */
	if (!strncasecmp(rel, "data/", 5) || !strncasecmp(rel, "data1/", 6) ||
	    !strncasecmp(rel, "dc_data/", 8) ||
	    !strncasecmp(rel, "save/", 5) || !strncasecmp(rel, "saves/", 6) ||
	    !strncasecmp(rel, "music/", 6) || !strncasecmp(rel, "sound/", 6) ||
	    !strncasecmp(rel, "sounds/", 7) || !strncasecmp(rel, "movies/", 7) ||
	    !strncasecmp(rel, "fmv/", 4))
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
		p->update = p->installed && p->checked && p->asset_name[0] && *p->version && strcmp(p->version, p->latest);
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

static void set_stage(const char *label)
{
	if (on_stage)
		on_stage(label);
}

/* fetches the repos' latest releases, all at once (one wget each), then retries the failed;
 * with `names` (a port of each repo), shows which one it is waiting for */
static void fetch_releases(char **repos, const char **names, int n)
{
	int pids[64];
	char f[64][512], part[64][600];
	const char *ca = wget_ca_cert();
	char ca_arg[700];
	if (ca)
		snprintf(ca_arg, sizeof ca_arg, "--ca-certificate=%s", ca);
	for (int i = 0; i < n && i < 64; i++) {
		char url[300];
		rel_path(repos[i], f[i], sizeof f[i]);
		snprintf(part[i], sizeof part[i], "%s.part", f[i]);
		snprintf(url, sizeof url, API "%s/releases/latest", repos[i]);
		unlink(f[i]);
		unlink(part[i]);
		const char *tok = token_for_repo(repos[i]);
		const char *argv[16];
		int argc = 0;
		char auth_hdr[300];
		if (tok) {
			snprintf(auth_hdr, sizeof auth_hdr, "Authorization: Bearer %s", tok);
			argv[argc++] = curl_bin();
			argv[argc++] = "-s";
			argv[argc++] = "-S";
			argv[argc++] = "-L";
			argv[argc++] = "-m";
			argv[argc++] = "15";
			if (ca) {
				argv[argc++] = "--cacert";
				argv[argc++] = ca;
			}
			argv[argc++] = "-H";
			argv[argc++] = auth_hdr;
			argv[argc++] = "-o";
			argv[argc++] = part[i];
			argv[argc++] = url;
			argv[argc] = NULL;
		} else {
			argv[argc++] = wget_bin();
			argv[argc++] = "-q";
			argv[argc++] = "-T";
			argv[argc++] = "15";
			if (ca)
				argv[argc++] = ca_arg;
			argv[argc++] = "-O";
			argv[argc++] = part[i];
			argv[argc++] = url;
			argv[argc] = NULL;
		}
		pids[i] = run_bg(argv, NULL);
	}
	for (int i = 0; i < n && i < 64; i++) {
		if (names) {
			char label[80];
			snprintf(label, sizeof label, "Checking %s", names[i]);
			set_stage(label);
			if (on_check_progress)
				on_check_progress(i, n);
		}
		char url[300];
		snprintf(url, sizeof url, API "%s/releases/latest", repos[i]);
		const char *tok = token_for_repo(repos[i]);
		if (!(run_wait(pids[i]) == 0 && file_size(part[i]) > 0 && rename(part[i], f[i]) == 0))
			fetch_auth(url, f[i], 0, tok, NULL);
		unlink(part[i]);
		char *text = read_file(f[i], NULL);
		cJSON *j = text ? cJSON_Parse(text) : NULL;
		free(text);
		if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "tag_name"))) {
			cJSON_Delete(j);
			j = NULL;
			/* If releases/latest failed (e.g. repo only has prereleases), try releases?per_page=5 */
			char url_all[300];
			snprintf(url_all, sizeof url_all, API "%s/releases?per_page=5", repos[i]);
			if (fetch_auth(url_all, f[i], 0, tok, NULL) == 0) {
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

static int refresh_catalog(void)
{
	char f[400];
	snprintf(f, sizeof f, "%s/catalog.json", WORK);
	if (fetch(CAT_URL, f, 0))
		return -1;
	if (same_file(f, CAT))
		return 0;
	/* only a catalog that loads replaces the one on the card */
	Port *old = ports;
	int oldn = nports;
	if (load_catalog(f)) {
		ports = old;
		nports = oldn;
		return -1;
	}
	copy_file(f, CAT);
	say("catalog updated");
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
		set_stage("Updating the catalog");
		refresh_catalog();
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
	else if (!strncmp(rel, "Roms/PORTS/Imgs/", 16))
		map_add(m, M_IMG, src, rel);
	else if (!strncmp(rel, "Roms/PORTS/Shortcuts/", 21) && strstr(rel, ".port")) {
		if (app)
			map_add(m, M_TEMPLATE, src, "");
		else {
			char bn[256];
			snprintf(bn, sizeof bn, "%s", base_name(rel));
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
	snprintf(name, sizeof name, "%s", base_name(tpl));
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
			!strncmp(line, "GameDir=", 8) || !strncmp(line, "GameDataFile=", 13))
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

static int apply_release(Port *p, const char *tag, const char *aname, const char *url_str,
	const char *api_url_str, const char *digest_str, long total_bytes)
{
	pm_error[0] = 0;
	shortcuts_changed = 0;
	char zip[600], stage[400], g[600], bk_stage[700];
	snprintf(zip, sizeof zip, "%s/%s", WORK, aname);
	snprintf(stage, sizeof stage, "%s/stage", WORK);
	snprintf(g, sizeof g, "%s", gdir(p));
	snprintf(bk_stage, sizeof bk_stage, "%s/bk_stage", WORK);
	remove_tree(bk_stage);

	const char *base = base_name(p->dir);
	char basebuf[256];
	snprintf(basebuf, sizeof basebuf, "%s", base);
	base = basebuf;

	set_stage("1/4 Downloading");
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
			unlink(zip);
			return fail("%s: the download's sha256 is wrong", p->name);
		}
	}
	else if (total_bytes > 0 && file_size(zip) != total_bytes) {
		unlink(zip);
		return fail("%s: the download's size is wrong", p->name);
	}
	remove_tree(stage);
	set_stage("2/4 Extracting");
	say_log("%s: extracting files...", p->name);
	if (on_extract_progress)
		on_extract_progress(0, 1);
	if (unpack(zip, stage)) {
		unlink(zip);
		return fail("%s: can't unpack the download", p->name);
	}
	if (on_extract_progress)
		on_extract_progress(1, 1);
	unlink(zip);

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
				Find f = {NULL, base, ""};
				walk_files(SC, find_cb, &f);
				if (!*f.found)
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
	/* then the backup, before anything is written: the port's backup folders, version and the files to be replaced */
	if (total_to_copy > 0 || is_dir(bk_stage)) {
		set_stage("3/4 Backup");
		say_log("%s: backing up...", p->name);
		mkdirs(bk_stage);
		char old_vf[700], bk_vf[1100];
		snprintf(old_vf, sizeof old_vf, "%s/.pm_version", g);
		snprintf(bk_vf, sizeof bk_vf, "%s/%s/.pm_version", bk_stage, p->dir);
		if (file_exists(old_vf) && !mkdirs_for(bk_vf))
			copy_file(old_vf, bk_vf);
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
			unlink(bk_tar);
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
		set_stage("4/4 Copying");
		say_log("%s: copying files...", p->name);
		if (on_copy_progress)
			on_copy_progress(0, total_to_copy);
	}
	int n = 0, failed = 0;
	for (int i = 0; i < m.n && !failed; i++) {
		MapEntry *e = &m.e[i];
		if (!replace[i])
			continue;
		char d[1000], h[65];
		snprintf(d, sizeof d, "%s/%s", SD, e->dst);
		if (sha256_file(e->src, h))
			h[0] = 0;
		if (mlen + strlen(e->dst) + 80 > mcap)
			newman = realloc(newman, mcap *= 2);
		if (copy_file(e->src, d)) {
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
				unlink(f);
			}
		if (d)
			closedir(d);
	}
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

	if (copy_tree(src_dir, SD)) {
		remove_tree(rstage);
		return fail("%s: failed to copy backup files to SD card", p->name);
	}
	remove_tree(rstage);

	char g[600];
	snprintf(g, sizeof g, "%s/%s", SD, p->dir);
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
			unlink(f);
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
	unlink(f);

	const char *tok = token_for_repo(p->repo);
	if (fetch_auth(url, f, 0, tok, NULL))
		return fail("%s: cannot fetch releases list from GitHub", p->name);

	char *text = read_file(f, NULL);
	unlink(f);
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
	unlink(f);
	snprintf(f, sizeof f, "%s/.miyoo64_version", g);
	unlink(f);
	snprintf(f, sizeof f, "%s/.pm_manifest", g);
	unlink(f);

	CleanCtx ctx = {0};
	ctx.p = p;
	snprintf(ctx.gdir, sizeof ctx.gdir, "%s", g);
	walk_files(g, clean_cb, &ctx);
	for (int i = 0; i < ctx.n; i++) {
		unlink(ctx.to_delete[i]);
		free(ctx.to_delete[i]);
	}
	say("%s: %d program file(s) removed", p->name, ctx.n);
	free(ctx.to_delete);

	rmdir_empty_subdirs(g);
	if (!dir_has_files(g))
		rmdir(g);
}

int pm_uninstall(Port *p, int keep_data)
{
	pm_error[0] = 0;
	if (!p->installed)
		return fail("%s: not installed", p->name);
	say("%s: removing port (keep_data=%d)", p->name, keep_data);

	/* 1. Remove shortcut in Roms/PORTS/Shortcuts/ */
	char sc[700];
	find_shortcut(p, sc, sizeof sc);
	if (*sc) {
		char sc_path[1000];
		snprintf(sc_path, sizeof sc_path, "%s/%s", SD, sc);
		say("%s: removing shortcut %s", p->name, sc);
		unlink(sc_path);

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
	unlink(img);
	if (*p->category) {
		snprintf(img, sizeof img, "%s/Roms/PORTS/Imgs/%s/%s.png", SD, p->category, p->name);
		unlink(img);
	}

	/* 3. Invalidate Onion's cached Ports list */
	DIR *d = opendir(SC);
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (strstr(e->d_name, "_cache") && strstr(e->d_name, ".db")) {
			char cf[600];
			snprintf(cf, sizeof cf, "%s/%s", SC, e->d_name);
			unlink(cf);
		}
	if (d)
		closedir(d);

	/* 4. Remove program files / directory */
	const char *g = gdir(p);
	if (!keep_data) {
		say("%s: deleting directory %s", p->name, g);
		remove_tree(g);
		if (legacy_app(p)) {
			char app[600];
			snprintf(app, sizeof app, "%s/App/%s", SD, base_name(p->dir));
			remove_tree(app);
		}
	} else {
		say("%s: cleaning program files, keeping game data", p->name);
		clean_port_files(p);
		if (legacy_app(p)) {
			char app[600];
			snprintf(app, sizeof app, "%s/App/%s", SD, base_name(p->dir));
			remove_tree(app);
		}
	}

	say("%s: removed", p->name);
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

void pm_info(const Port *p, char *buf, int size)
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

	/* 2. Latest */
	if (!p->checked) {
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
	} else if (p->nrequired == 0 && (!p->rom || !*p->rom)) {
		append_info(buf, size, &n, CLR_DIM "Files:     " CLR_GOOD "OK (none required)\n");
	} else {
		append_info(buf, size, &n, CLR_DIM "Files:     " CLR_DIM "not found yet\n");
	}

	append_info(buf, size, &n, CLR_DIM "Source:    " CLR_TEXT "github.com/%s%s\n",
		p->repo, *p->asset ? "" : " (no releases yet)");

	/* 5. Instructions */
	if (p->get && *p->get)
		append_info(buf, size, &n, "\n" CLR_DIM "Instructions:\n" CLR_TEXT "%s\n", p->get);
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
	if (!strcmp(cur_v, tag_v))
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
	unlink(zip);
	say("Port Manager: downloading %s...", pm_self_latest_version);
	if (pm_self_asset_size > 0) {
		if (pm_self_asset_size >= 1024 * 1024)
			say("download size: %.1f MB", (double)pm_self_asset_size / (1024.0 * 1024.0));
		else
			say("download size: %ld KB", (pm_self_asset_size + 1023) / 1024);
	}

	const char *tok = token_for_repo(PM_REPO);
	if (fetch_auth(pm_self_asset_url, zip, pm_self_asset_size, tok, NULL) != 0) {
		unlink(zip);
		return fail("%s: download failed", "Port Manager");
	}

	if (pm_self_asset_size > 0 && file_size(zip) != pm_self_asset_size) {
		unlink(zip);
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
		unlink(zip);
		remove_tree(stage);
		return fail("%s: could not unpack update archive", "Port Manager");
	}
	if (on_extract_progress)
		on_extract_progress(1, 1);
	unlink(zip);


	/* 4. Copy files to SD */
	say("Port Manager: applying update...");
	char stage_app[700], stage_roms[700];
	snprintf(stage_app, sizeof stage_app, "%s/App", stage);
	snprintf(stage_roms, sizeof stage_roms, "%s/Roms", stage);

	if (is_dir(stage_app)) {
		char dst_app[600];
		snprintf(dst_app, sizeof dst_app, "%s/App", SD);
		copy_tree(stage_app, dst_app);
	} else {
		copy_tree(stage, HERE);
	}

	if (is_dir(stage_roms)) {
		char dst_roms[600];
		snprintf(dst_roms, sizeof dst_roms, "%s/Roms", SD);
		copy_tree(stage_roms, dst_roms);
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
			unlink(f);
		}
	}
	if (d)
		closedir(d);

	remove_tree(stage);
	say("Port Manager: updated to %s successfully!", pm_self_latest_version);
	return 0;
}

