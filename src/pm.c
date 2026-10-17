#include "pm.h"

#include <dirent.h>
#include <fnmatch.h>
#include <net/if.h>
#include <netinet/in.h>
#include <regex.h>
#include <signal.h>
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

#define CAT_URL "https://raw.githubusercontent.com/ksielick/portmaster-miyoo/main/catalog.json"
#define API "https://api.github.com/repos/"

Port *ports;
int nports;
char pm_error[512];

static char SD[256], HERE[512], CAT[600], WORK[300], SC[300], STAMP[32];
static int shortcuts_changed;

/* --- the catalog --- */

static char *str(const cJSON *o, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return strdup(cJSON_IsString(v) ? v->valuestring : "");
}

static char **strs(const cJSON *o, const char *key, int *n)
{
	const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key), *v;
	char **r = calloc(cJSON_GetArraySize(a) + 1, sizeof *r);
	*n = 0;
	cJSON_ArrayForEach(v, a)
		if (cJSON_IsString(v))
			r[(*n)++] = strdup(v->valuestring);
	return r;
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
		Port *p = &ports[nports++];
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
		p->sc_gdf = str(sc, "GameDataFile");
		p->sc_run = str(sc, "run");
		p->skip = strs(e, "skip", &p->nskip);
		p->backup = strs(e, "backup", &p->nbackup);
		p->required = strs(data, "required", &p->nrequired);
		p->required_any = strs(data, "required_any", &p->nrequired_any);
	}
	cJSON_Delete(root);
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
	struct stat st;
	if (stat(path, &st) == 0 && (st.st_mode & 0111))
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
		if (req[reqlen - 1] == '/' && !strncasecmp(rel, req, reqlen))
			return 1;
		if (!fnmatch(req, rel, FNM_CASEFOLD) || !fnmatch(req, base_name(rel), FNM_CASEFOLD))
			return 1;
	}

	/* 4. Catalog required_any files */
	for (int i = 0; i < p->nrequired_any; i++) {
		const char *req = p->required_any[i];
		size_t reqlen = strlen(req);
		if (req[reqlen - 1] == '/' && !strncasecmp(rel, req, reqlen))
			return 1;
		if (!fnmatch(req, rel, FNM_CASEFOLD) || !fnmatch(req, base_name(rel), FNM_CASEFOLD))
			return 1;
	}

	/* 5. Catalog backup directories / files (e.g. saves, config) */
	for (int i = 0; i < p->nbackup; i++) {
		const char *bk = p->backup[i];
		size_t bklen = strlen(bk);
		if (bk[bklen - 1] == '/' && !strncasecmp(rel, bk, bklen))
			return 1;
		if (!fnmatch(bk, rel, FNM_CASEFOLD) || !fnmatch(bk, base_name(rel), FNM_CASEFOLD))
			return 1;
	}

	/* 6. Standard save / data folder names */
	if (!strncasecmp(rel, "data/", 5) || !strncasecmp(rel, "data1/", 6) ||
	    !strncasecmp(rel, "save/", 5) || !strncasecmp(rel, "saves/", 6) ||
	    !strncasecmp(rel, "music/", 6) || !strncasecmp(rel, "sound/", 6) ||
	    !strncasecmp(rel, "sounds/", 7) || !strncasecmp(rel, "movies/", 7) ||
	    !strncasecmp(rel, "fmv/", 4))
		return 1;

	/* 7. Documentation and text files are program/release files unless required above */
	if (ends_with(rel, ".txt") || ends_with(rel, ".md") || ends_with(rel, ".pdf") || ends_with(rel, ".doc") ||
	    !strncasecmp(base_name(rel), "readme", 6) || !strncasecmp(base_name(rel), "license", 7) ||
	    !strncasecmp(base_name(rel), "changelog", 9) || !strncasecmp(base_name(rel), "copying", 7))
		return 0;

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
		if (!file_exists(r))
			add_missing(p, p->rom);
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
		p->update = p->installed && p->checked && *p->version && strcmp(p->version, p->latest);
	}
	say("scan: %d/%d ports installed", ninst, nports);
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
	if (load_catalog(CAT))
		return -1;
	pm_scan();
	return 0;
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
	if (fetch("https://api.github.com/rate_limit", rl)) {
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

/* fetches the repos' latest releases, all at once (one wget each), then retries the failed */
static void fetch_releases(char **repos, int n)
{
	int pids[64];
	char f[64][512], part[64][520];
	for (int i = 0; i < n && i < 64; i++) {
		char url[300];
		rel_path(repos[i], f[i], sizeof f[i]);
		snprintf(part[i], sizeof part[i], "%s.part", f[i]);
		snprintf(url, sizeof url, API "%s/releases/latest", repos[i]);
		unlink(f[i]);
		unlink(part[i]);
		const char *argv[] = {wget_bin(), "-q", "-T", "15", "-O", part[i], url, NULL};
		pids[i] = run_bg(argv, NULL);
	}
	for (int i = 0; i < n && i < 64; i++) {
		char url[300];
		snprintf(url, sizeof url, API "%s/releases/latest", repos[i]);
		if (!(run_wait(pids[i]) == 0 && file_size(part[i]) > 0 && rename(part[i], f[i]) == 0))
			fetch(url, f[i]);
		unlink(part[i]);
		char *text = read_file(f[i], NULL);
		cJSON *j = text ? cJSON_Parse(text) : NULL;
		free(text);
		if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "tag_name"))) {
			cJSON_Delete(j);
			j = NULL;
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
	if (fetch(CAT_URL, f))
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

/* checks `only`'s repo (any port, installed or not), or every installed port's */
static int checks(const Port *p, const Port *only)
{
	return *p->asset && (only ? !strcmp(p->repo, only->repo) : p->installed);
}

int pm_check(const Port *only)
{
	pm_error[0] = 0;
	/* a new catalog reallocates ports: only for the whole check, `only` must stay valid */
	if (!only)
		refresh_catalog();
	pm_scan();
	char *repos[64];
	int n = 0;
	for (int i = 0; i < nports; i++) {
		Port *p = &ports[i];
		if (!checks(p, only))
			continue;
		int seen = 0;
		for (int j = 0; j < n; j++)
			seen |= !strcmp(repos[j], p->repo);
		if (!seen && n < 64)
			repos[n++] = p->repo;
	}
	if (net_check(n)) {
		say("NONET %s", pm_error);
		return -1;
	}
	fetch_releases(repos, n);
	int failed = 0;
	for (int i = 0; i < nports; i++) {
		Port *p = &ports[i];
		Release *r = release_of(p->repo);
		p->checked = r && r->json;
		if (p->checked)
			snprintf(p->latest, sizeof p->latest, "%s",
				cJSON_GetObjectItemCaseSensitive(r->json, "tag_name")->valuestring);
		else if (checks(p, only)) {
			say("ERROR %s: no release info", p->name);
			failed++;
		}
	}
	pm_scan();
	if (failed) {
		snprintf(pm_error, sizeof pm_error, "No release info for %d port(s): the connection broke off. Try again.", failed);
		return -1;
	}
	return 0;
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
	else if (!app && !strncmp(rel, "Roms/PORTS/Games/", 17))
		map_add(m, M_GAME, src, rel);
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
	int n = snprintf(text, sizeof text, "#!/bin/sh\n# %s: runs %s/%s. Written by Port manager (portmaster-miyoo).\n",
		name, p->dir, p->sc_run);
	for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
		size_t k = 0;
		while ((line[k] >= 'A' && line[k] <= 'Z') || (line[k] >= 'a' && line[k] <= 'z') || line[k] == '_' ||
			(k && line[k] >= '0' && line[k] <= '9'))
			k++;
		if (!k || line[k] != '=' || strstr(line, "App/") || !strncmp(line, "APP=", 4) ||
			!strncmp(line, "GameDir=", 8) || !strncmp(line, "GameDataFile=", 13))
			continue;
		n += snprintf(text + n, sizeof text - n, "%s\n", line);
	}
	free(t);
	n += snprintf(text + n, sizeof text - n, "GameDir=\"%s\"\nGameDataFile=\"%s\"\ncd \"%s\" || exit 1\nexec sh \"%s/%s\"\n",
		base_name(p->dir), p->sc_gdf, g, g, p->sc_run);
	return write_file(out, text, n);
}

static int unpack(const char *file, const char *to)
{
	mkdirs(to);
	if (strstr(file, ".zip")) {
		mz_zip_archive z;
		memset(&z, 0, sizeof z);
		if (!mz_zip_reader_init_file(&z, file, 0))
			return -1;
		int ok = 1;
		for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) {
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
		}
		mz_zip_reader_end(&z);
		return ok ? 0 : -1;
	}
	char o[1100];
	snprintf(o, sizeof o, "-o%s", to);
	const char *x7[] = {"7z", "x", "-y", o, file, NULL};
	const char *tgz[] = {"tar", "-xzf", file, "-C", to, NULL};
	const char *tar[] = {"tar", "-xf", file, "-C", to, NULL};
	if (strstr(file, ".7z"))
		return run(x7, NULL) ? -1 : 0;
	if (strstr(file, ".tar.gz") || strstr(file, ".tgz"))
		return run(tgz, NULL) ? -1 : 0;
	if (strstr(file, ".tar"))
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

int pm_update(Port *p)
{
	pm_error[0] = 0;
	shortcuts_changed = 0;
	if (!*p->asset)
		return fail("%s: no releases yet", p->name);
	Release *r = release_of(p->repo);
	if (!r || !r->json) {
		char *repo[] = {p->repo};
		if (net_check(1))
			return -1;
		fetch_releases(repo, 1);
		r = release_of(p->repo);
		if (!r || !r->json)
			return fail("%s: no release info (no connection?)", p->name);
	}
	const char *tag = cJSON_GetObjectItemCaseSensitive(r->json, "tag_name")->valuestring;

	/* the release's file */
	regex_t re;
	if (regcomp(&re, p->asset, REG_EXTENDED | REG_NOSUB))
		return fail("%s: bad asset pattern in the catalog", p->name);
	const cJSON *a, *asset = NULL;
	cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(r->json, "assets")) {
		const cJSON *nm = cJSON_GetObjectItemCaseSensitive(a, "name");
		if (cJSON_IsString(nm) && !regexec(&re, nm->valuestring, 0, NULL, 0)) {
			asset = a;
			break;
		}
	}
	regfree(&re);
	if (!asset)
		return fail("%s: the release has no matching file", p->name);
	const char *aname = cJSON_GetObjectItemCaseSensitive(asset, "name")->valuestring;
	const cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
	const cJSON *digest = cJSON_GetObjectItemCaseSensitive(asset, "digest");
	const cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
	char zip[600], stage[400], g[600], bk[800];
	snprintf(zip, sizeof zip, "%s/%s", WORK, aname);
	snprintf(stage, sizeof stage, "%s/stage", WORK);
	snprintf(g, sizeof g, "%s", gdir(p));
	snprintf(bk, sizeof bk, "%s/Backups/PortManager/%s-%s-before-%s", SD, STAMP, p->id, tag);
	const char *base = base_name(p->dir);
	char basebuf[256];
	snprintf(basebuf, sizeof basebuf, "%s", base);
	base = basebuf;

	say("%s: %s -> %s, downloading %s (%d KB)", p->name, *p->version ? p->version : "?", tag, aname,
		cJSON_IsNumber(size) ? size->valueint / 1024 : 0);
	if (!cJSON_IsString(url) || fetch(url->valuestring, zip))
		return fail("%s: download failed", p->name);
	if (cJSON_IsString(digest) && !strncmp(digest->valuestring, "sha256:", 7)) {
		char h[65];
		if (sha256_file(zip, h) || strcmp(h, digest->valuestring + 7)) {
			unlink(zip);
			return fail("%s: the download's sha256 is wrong", p->name);
		}
	}
	else if (cJSON_IsNumber(size) && file_size(zip) != (long)size->valuedouble) {
		unlink(zip);
		return fail("%s: the download's size is wrong", p->name);
	}
	remove_tree(stage);
	if (unpack(zip, stage)) {
		unlink(zip);
		return fail("%s: can't unpack the download", p->name);
	}
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
		snprintf(bcfg, sizeof bcfg, "%s/App/%s/config.json", bk, base);
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
	int n = 0, saved = 0, failed = 0;
	for (int i = 0; i < m.n && !failed; i++) {
		MapEntry *e = &m.e[i];
		if (e->kind == M_TEMPLATE)
			continue;
		char d[1000], h[65];
		snprintf(d, sizeof d, "%s/%s", SD, e->dst);
		sha256_file(e->src, h);
		if (mlen + strlen(e->dst) + 80 > mcap)
			newman = realloc(newman, mcap *= 2);
		if (file_exists(d)) {
			if (same_file(e->src, d)) {
				mlen += sprintf(newman + mlen, "%s %s\n", h, e->dst);
				continue;
			}
			if (e->kind != M_GAME) {
				char oh[65];
				sha256_file(d, oh);
				int ours = manifest_has(oldman, oh, e->dst);
				if (e->kind == M_SHORTCUT) {
					char *t = read_file(d, NULL);
					ours |= t && (strstr(t, "Written by update_miyoo64.sh") || strstr(t, "Written by Port manager"));
					free(t);
				}
				if (!ours) {
					say("%s: kept your %s", p->name, e->dst);
					continue;
				}
			}
		}
		/* before the first replaced file: the port's backup folders */
		if (!saved) {
			saved = 1;
			for (int b = 0; b < p->nbackup; b++) {
				char from[800], to[1100];
				snprintf(from, sizeof from, "%s/%s", g, p->backup[b]);
				snprintf(to, sizeof to, "%s/%s/%s", bk, p->dir, p->backup[b]);
				if (to[strlen(to) - 1] == '/')
					to[strlen(to) - 1] = 0;
				if (is_dir(from) && copy_tree(from, to) == 0)
					say("%s: saved %s/%s to the backup", p->name, p->dir, p->backup[b]);
			}
		}
		if (file_exists(d)) {
			char b[1100];
			snprintf(b, sizeof b, "%s/%s", bk, e->dst);
			copy_file(d, b);
		}
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
		say("%s: updated %s", p->name, e->dst);
		n++;
	}
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
	if (saved)
		say("backup: %s", bk + strlen(SD) + 1);
	pm_scan();
	return 0;
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

void pm_info(const Port *p, char *buf, int size)
{
	int n = snprintf(buf, size, "%s (%s)\n", p->name, p->category);
	if (p->installed) {
		n += snprintf(buf + n, size - n, "Installed: %s, in %s\n", *p->version ? p->version : "version unknown", p->dir);
		if (p->checked && *p->asset)
			n += snprintf(buf + n, size - n, "Latest: %s%s\n", p->latest,
				!*p->version ? " - yours unknown (installed without the port manager); A installs it" :
				p->update ? " - update available" : " - up to date");
		else if (*p->asset)
			n += snprintf(buf + n, size - n, "Latest: not checked (Y)\n");
		n += snprintf(buf + n, size - n, *p->missing ? "Game files missing: %s\n" : "Game files: OK\n", p->missing);
	}
	else {
		n += snprintf(buf + n, size - n, "Not installed (would go to %s)\n", p->dir);
		if (p->checked && *p->asset)
			n += snprintf(buf + n, size - n, "Latest: %s\n", p->latest);
		if (dir_has_files(gdir(p)))
			n += snprintf(buf + n, size - n, *p->missing ? "Game files missing: %s\n" : "Game files: OK\n", p->missing);
	}
	n += snprintf(buf + n, size - n, "From: github.com/%s%s\n\n", p->repo, *p->asset ? "" : " (no releases yet)");
	if (*p->get)
		snprintf(buf + n, size - n, "%s\n", p->get);
}
