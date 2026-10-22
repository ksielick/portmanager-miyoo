/* Port Manager for OnionOS on the Miyoo Mini (portmanager-miyoo): lists the installed ports
 * and the whole catalog, shows a port's details and how to add its game files, checks the
 * authors' GitHub releases and installs updates. https://github.com/ksielick/portmanager-miyoo
 * Buttons: Up/Down choose, L1/R1 a page, A details, X all ports, Y check for updates,
 * Start update all, B back (exit on the first screen). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#include "gfx.h"
#include "pm.h"
#include "util.h"

static char **g_argv = NULL;
static char g_self_bin[512] = {0};
/* the check runs right away: 1 = --check, started again after a self-update; 2 = no catalog
 * on the card yet (Port Manager is checked first, as with Y) */
static int g_check_at_start;
/* Start on a port's page: the game is set to run, so every screen closes */
static int g_launch;
/* the check page's first line: what became of Port Manager itself */
static char g_pm_line[120];


#define TOPH 48
#define FOOTH 40
#define ROWH 26
#define LIST_Y (TOPH + 6)
#define LIST_ROWS ((SCR_H - TOPH - FOOTH - 12) / ROWH)
#define COLS ((SCR_W - 2 * 16 - 12) / CW)
#define TEXT_ROWS ((SCR_H - TOPH - FOOTH - 16) / (CW + 6))

/* --- drawing --- */

static void header(const char *title, const char *right)
{
	gfx_fill(0, 0, SCR_W, SCR_H, C_BG);
	gfx_fill(0, 0, SCR_W, TOPH, C_BAR);
	int max_left = (right && *right) ? COLS - (int)strlen(right) - 2 : COLS;
	if (max_left < 10) max_left = 10;
	gfx_text(16, (TOPH - CW) / 2, title, max_left, C_TEXT);
	if (right && *right)
		gfx_text(SCR_W - 16 - (int)strlen(right) * CW, (TOPH - CW) / 2, right, (int)strlen(right), C_DIM);
}

static void footer(const char *hints)
{
	gfx_fill(0, SCR_H - FOOTH, SCR_W, FOOTH, C_BAR);
	gfx_text(16, SCR_H - FOOTH + (FOOTH - CW) / 2, hints, COLS + 1, C_DIM);
}

static void scrollbar(int top, int shown, int total, int y0, int h)
{
	if (total <= shown)
		return;
	int th = h * shown / total, ty = y0 + (h - th) * top / (total - shown);
	gfx_fill(SCR_W - 10, y0, 4, h, C_BAR);
	gfx_fill(SCR_W - 12, ty, 8, th < 16 ? 16 : th, C_ACCENT);
}

/* --- wrapped text --- */

typedef struct {
	char **l;
	int n, cap;
} Lines;

static void lines_push(Lines *t, const char *s, size_t n)
{
	if (t->n == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 64;
		t->l = realloc(t->l, t->cap * sizeof *t->l);
	}
	t->l[t->n++] = strndup(s, n);
}

/* adds text wrapped at word boundaries; UTF-8 shows as '?' */
static void lines_add(Lines *t, const char *text)
{
	char *a = strdup(text), *d = a;
	for (const unsigned char *s = (const unsigned char *)text; *s; s++)
		if (*s == '\t')
			*d++ = ' ';
		else if (*s < 0x80 && *s != '\r')
			*d++ = *s;
		else if (*s >= 0xC0)
			*d++ = '?';
	*d = 0;
	for (char *p = a;;) {
		char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p), i = 0;
		if (len == 0)
			lines_push(t, "", 0);
		char cur_clr = 0;
		while (i < len) {
			while (i < len && p[i] == ' ')
				i++;
			if (i >= len)
				break;
			size_t start = i, last = 0, j = i, cols = 0;
			while (j < len && cols < COLS) {
				unsigned char ch = (unsigned char)p[j];
				if (ch >= 1 && ch <= 6) {
					cur_clr = (char)ch;
					j++;
					continue;
				}
				if (p[j] == ' ')
					last = j;
				cols++;
				j++;
			}
			size_t end = (j >= len) ? len : (last > start ? last : j);
			if (cur_clr && ((unsigned char)p[start] < 1 || (unsigned char)p[start] > 6)) {
				char tmp[COLS * 2 + 16];
				tmp[0] = cur_clr;
				size_t chunk_len = end - start;
				if (chunk_len > sizeof(tmp) - 2)
					chunk_len = sizeof(tmp) - 2;
				memcpy(tmp + 1, p + start, chunk_len);
				lines_push(t, tmp, chunk_len + 1);
			} else {
				lines_push(t, p + start, end - start);
			}
			i = end;
		}
		if (!e)
			break;
		p = e + 1;
	}
	free(a);
}

static void lines_free(Lines *t)
{
	for (int i = 0; i < t->n; i++)
		free(t->l[i]);
	free(t->l);
	memset(t, 0, sizeof *t);
}

static void draw_lines_limit(const Lines *t, int top, int max_rows)
{
	for (int r = 0; r < max_rows && top + r < t->n; r++)
		gfx_text(16, TOPH + 8 + r * (CW + 6), t->l[top + r], COLS, C_TEXT);
	scrollbar(top, max_rows, t->n, TOPH + 8, max_rows * (CW + 6));
}

static void draw_lines(const Lines *t, int top)
{
	draw_lines_limit(t, top, TEXT_ROWS);
}

/* a scrollable page; returns the key that closed it (B, or one of `keys`, 0-terminated).
 * If start_top < 0, starts scrolled all the way to the bottom. */
static int view_at(const char *title, const char *right, const Lines *t, const char *hints, const int *keys, int start_top)
{
	int last = t->n > TEXT_ROWS ? t->n - TEXT_ROWS : 0;
	int top = (start_top < 0 || start_top > last) ? last : start_top;
	for (;;) {
		header(title, right);
		draw_lines(t, top);
		footer(hints);
		gfx_flip();
		int k = input_key(NULL);
		if (k < 0 || k == K_B || k == K_MENU)
			return k;
		for (const int *c = keys; c && *c; c++)
			if (k == *c)
				return k;
		if (k == K_UP) top--;
		if (k == K_DOWN) top++;
		if (k == K_L1 || k == K_LEFT) top -= TEXT_ROWS - 1;
		if (k == K_R1 || k == K_RIGHT) top += TEXT_ROWS - 1;
		if (top > last) top = last;
		if (top < 0) top = 0;
	}
}

static int view(const char *title, const char *right, const Lines *t, const char *hints, const int *keys)
{
	return view_at(title, right, t, hints, keys, 0);
}

/* --- the progress page: pm's log lines appear as they come --- */

static Lines progress;
static Lines installed_files;
static const char *progress_title;
static char progress_right[24];
/* the step running now and its bar: one place under the log lines, replaced step by step */
static char status_label[48], status_bar[80];
/* a label ending in ':' ("Catalog:") has its bar on the same line */
static int status_inline;
/* "<port name>: " is left out on the screen (the log keeps it) */
static const char *screen_prefix;

static void make_ascii_bar(char *buf, size_t size, long cur, long total)
{
	int bar_width = 16;
	char bar[32];
	int pct = 0;
	if (total > 0) {
		pct = (int)((double)cur / total * 100.0);
		if (pct > 100) pct = 100;
		if (pct < 0) pct = 0;
		int fill = (int)((double)cur / total * bar_width);
		if (fill > bar_width) fill = bar_width;

		bar[0] = '[';
		for (int i = 0; i < bar_width; i++) {
			if (i < fill - 1)
				bar[1 + i] = '=';
			else if (i == fill - 1 && fill < bar_width)
				bar[1 + i] = '>';
			else if (i == fill - 1)
				bar[1 + i] = '=';
			else
				bar[1 + i] = ' ';
		}
		bar[1 + bar_width] = ']';
		bar[2 + bar_width] = 0;

		double cur_mb = (double)cur / (1024.0 * 1024.0);
		double tot_mb = (double)total / (1024.0 * 1024.0);
		if (tot_mb >= 1.0)
			snprintf(buf, size, CLR_GOOD "%s" CLR_TEXT " %3d%%  %.1f/%.1f MB",
				bar, pct, cur_mb, tot_mb);
		else
			snprintf(buf, size, CLR_GOOD "%s" CLR_TEXT " %3d%%  %ld/%ld KB",
				bar, pct, (cur + 1023) / 1024, (total + 1023) / 1024);
	} else {
		double cur_mb = (double)cur / (1024.0 * 1024.0);
		snprintf(buf, size, CLR_GOOD "[===>            ]" CLR_TEXT "  %.1f MB", cur_mb);
	}
}

static void make_ascii_count_bar(char *buf, size_t size, long cur, long total, const char *unit)
{
	int bar_width = 14;
	char bar[32];
	int pct = 0;
	if (total > 0) {
		pct = (int)((double)cur / total * 100.0);
		if (pct > 100) pct = 100;
		if (pct < 0) pct = 0;
		int fill = (int)((double)cur / total * bar_width);
		if (fill > bar_width) fill = bar_width;

		bar[0] = '[';
		for (int i = 0; i < bar_width; i++) {
			if (i < fill - 1)
				bar[1 + i] = '=';
			else if (i == fill - 1 && fill < bar_width)
				bar[1 + i] = '>';
			else if (i == fill - 1)
				bar[1 + i] = '=';
			else
				bar[1 + i] = ' ';
		}
		bar[1 + bar_width] = ']';
		bar[2 + bar_width] = 0;

		if (unit)
			snprintf(buf, size, CLR_GOOD "%s" CLR_TEXT " %3d%%  %ld/%ld %s", bar, pct, cur, total, unit);
		else /* the percentage only, to fit after a label */
			snprintf(buf, size, CLR_GOOD "%s" CLR_TEXT " %3d%%", bar, pct);
	} else {
		snprintf(buf, size, CLR_GOOD "[===>          ]" CLR_TEXT "  %ld %s", cur, unit ? unit : "");
	}
}

static void progress_draw(const char *hints)
{
	int rows = TEXT_ROWS - (*status_label != 0) - (*status_bar != 0 && !status_inline);
	int top = progress.n > rows ? progress.n - rows : 0;
	header(progress_title, progress_right);
	draw_lines_limit(&progress, top, rows);
	int y = TOPH + 8 + (progress.n - top) * (CW + 6);
	if (status_inline) {
		int n = (int)strlen(status_label) + 1;
		gfx_text(16, y, status_label, COLS, C_TEXT);
		if (*status_bar)
			gfx_text(16 + n * CW, y, status_bar, COLS - n, C_TEXT);
		y += CW + 6;
	}
	else {
		if (*status_label) {
			gfx_text(16, y, status_label, COLS, C_DIM);
			y += CW + 6;
		}
		if (*status_bar)
			gfx_text(16, y, status_bar, COLS, C_TEXT);
	}
	footer(hints);
	gfx_flip();
}

static void status_clear(void)
{
	status_label[0] = status_bar[0] = 0;
}

static void stage_changed(const char *label)
{
	snprintf(status_label, sizeof status_label, "%s", label);
	size_t n = strlen(status_label);
	status_inline = n && status_label[n - 1] == ':';
	status_bar[0] = 0;
	progress_draw("Please wait...");
}

static void set_bar(const char *s)
{
	snprintf(status_bar, sizeof status_bar, "%s", s);
	progress_draw("Please wait...");
}

static const char *unprefixed(const char *line)
{
	size_t n = screen_prefix ? strlen(screen_prefix) : 0;
	if (n && !strncmp(line, screen_prefix, n) && line[n] == ':' && line[n + 1] == ' ')
		return line + n + 2;
	return line;
}

static void progress_sink(const char *line)
{
	/* lines meant for the log only */
	if (!strncmp(line, "NONET ", 6) || !strncmp(line, "ERROR ", 6) || !strncmp(line, "--- ", 4))
		return;
	lines_add(&progress, unprefixed(line));
	progress_draw("Please wait...");
}

static void download_progress(long current, long total)
{
	if (total <= 0 && current <= 0) {
		set_bar("");
		return;
	}
	char s[80];
	if (status_inline)
		make_ascii_count_bar(s, sizeof s, current > total ? total : current, total, NULL);
	else
		make_ascii_bar(s, sizeof s, current, total);
	set_bar(s);
}

static void count_progress(long current, long total)
{
	if (total <= 0) {
		set_bar("");
		return;
	}
	/* every redraw waits for the panel's refresh: at most 4 a second, not one per 5 files
	 * (unpacking 1250 files: 23.5 s -> 20.9 s) */
	static long long last_ms;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
	if (current > 0 && current < total && now - last_ms < 250)
		return;
	last_ms = now;
	char s[80];
	make_ascii_count_bar(s, sizeof s, current, total, "files");
	set_bar(s);
}

static void check_progress(long current, long total)
{
	char s[80];
	make_ascii_count_bar(s, sizeof s, current, total, "ports");
	set_bar(s);
}

static void file_installed(const char *path)
{
	lines_add(&installed_files, path);
}

static void progress_begin(const char *title)
{
	lines_free(&progress);
	lines_free(&installed_files);
	progress_title = title;
	progress_right[0] = 0;
	screen_prefix = NULL;
	status_clear();
	log_sink = progress_sink;
	on_download_progress = download_progress;
	on_extract_progress = count_progress;
	on_copy_progress = count_progress;
	on_check_progress = check_progress;
	on_file_installed = file_installed;
	on_stage = stage_changed;
	progress_draw("Please wait...");
}

/* shows the result; returns the key that closed it (B, or one of `keys`) */
static void progress_stop(void)
{
	log_sink = NULL;
	on_download_progress = NULL;
	on_extract_progress = NULL;
	on_copy_progress = NULL;
	on_check_progress = NULL;
	on_file_installed = NULL;
	on_stage = NULL;
	screen_prefix = NULL;
	status_clear();
}

static int progress_end_at(const char *hints, const int *keys, int start_top)
{
	progress_stop();

	char custom_hints[80];
	if (!hints) {
		if (installed_files.n > 0)
			snprintf(custom_hints, sizeof custom_hints, "Select: files   Up/Down   B: back");
		else
			snprintf(custom_hints, sizeof custom_hints, "Up/Down   B: back");
		hints = custom_hints;
	}

	int handled_keys[8];
	int nhk = 0;
	if (keys) {
		for (const int *k = keys; *k; k++)
			handled_keys[nhk++] = *k;
	}
	if (installed_files.n > 0)
		handled_keys[nhk++] = K_SELECT;
	handled_keys[nhk] = 0;

	for (;;) {
		int k = view_at(progress_title, progress_right, &progress, hints, handled_keys, start_top);
		if (k == K_SELECT && installed_files.n > 0) {
			view_at("Installed files", NULL, &installed_files, "Up/Down   B: back", NULL, 0);
			continue;
		}
		lines_free(&progress);
		lines_free(&installed_files);
		return k;
	}
}

static int progress_end(const char *hints, const int *keys)
{
	return progress_end_at(hints, keys, -1);
}

static int port_ok;

/* one port's install or update: its name once, then lines without it */
static char item_name[200];

/* a port, or anything else worked on in turn (a backup), in accent colour */
static void item_begin(const char *name)
{
	if (progress.n)
		lines_add(&progress, "");
	snprintf(item_name, sizeof item_name, "%s", name);
	char s[220];
	snprintf(s, sizeof s, CLR_ACCENT "%s", item_name);
	lines_add(&progress, s);
	screen_prefix = item_name;
	port_ok = 1;
	progress_draw("Please wait...");
}

static void port_begin(const Port *p)
{
	item_begin(p->name);
}

static void port_failed(void)
{
	char s[sizeof pm_error + 4];
	snprintf(s, sizeof s, CLR_BAD "%s", unprefixed(pm_error));
	lines_add(&progress, s);
	port_ok = 0;
}

/* the running bar goes; a finished port keeps a full one (owner, 2026-10-19) */
static void port_end(void)
{
	if (port_ok)
		lines_add(&progress, CLR_GOOD "[==============]" CLR_TEXT " 100%  done");
	screen_prefix = NULL;
	status_clear();
	progress_draw("Please wait...");
}

/* --- actions --- */

static int count_updates(void)
{
	int n = 0;
	for (int i = 0; i < nports; i++)
		n += ports[i].update;
	return n;
}

/* every installed port, or only one (from its details); returns the key that closed the
 * result: A (install `only`'s release) or Start (update all) when offered there, else B */
static int do_check(const Port *only)
{
	static const int key_a[] = {K_A, 0}, key_start[] = {K_START, 0},
		key_both[] = {K_START, K_SELECT, 0}, key_sel[] = {K_SELECT, 0};
	const char *hints = NULL;
	const int *keys = NULL;
	progress_begin("Checking for updates");
	say(only ? "--- check %s" : "--- check", only ? only->id : "");
	/* in order: Port Manager, the catalog, the ports (pm_check says the last two) */
	if (only)
		say("Checking %s...", only->name);
	else if (pm_self_update_available) {
		say_log("Port Manager %s is available", pm_self_latest_version);
		say_screen(CLR_GOOD "Port Manager %s is available" CLR_TEXT, pm_self_latest_version);
	}
	else if (*g_pm_line)
		say("%s", g_pm_line);
	g_pm_line[0] = 0;
	if (pm_check(only)) {
		lines_add(&progress, "");
		lines_add(&progress, "Can't check for updates:");
		lines_add(&progress, pm_error);
	}
	else if (only) {
		char s[350];
		char sz_line[64] = "";
		if (only->asset_size > 0) {
			if (only->asset_size >= 1024 * 1024)
				snprintf(sz_line, sizeof sz_line, "\nDownload size: %.1f MB", (double)only->asset_size / (1024.0 * 1024.0));
			else
				snprintf(sz_line, sizeof sz_line, "\nDownload size: %ld KB", (only->asset_size + 1023) / 1024);
		}
		if (only->update) {
			snprintf(s, sizeof s, "%s: %s -> %s%s", only->name, only->version, only->latest, sz_line);
			hints = "Up/Down  A: update  B: back";
			keys = key_a;
		}
		else if (only->installed && !*only->version) {
			snprintf(s, sizeof s, "%s: the latest release is %s.%s\nThe installed version is unknown "
				"(installed without the port manager).\n\nA: install %s over it (files that are "
				"the same stay as they are).", only->name, only->latest, sz_line, only->latest);
			hints = "Up/Down  A: install  B: back";
			keys = key_a;
		}
		else if (only->installed)
			snprintf(s, sizeof s, "%s is up to date (%s).", only->name, only->latest);
		else {
			snprintf(s, sizeof s, "%s: the latest release is %s.%s\n\nA: install it.", only->name, only->latest, sz_line);
			hints = "Up/Down  A: install  B: back";
			keys = key_a;
		}
		lines_add(&progress, "");
		lines_add(&progress, s);
		const char *notes = pm_release_notes(only);
		if (notes) {
			while (*notes == ' ' || *notes == '\t' || *notes == '\r' || *notes == '\n')
				notes++;
			if (*notes) {
				lines_add(&progress, "");
				lines_add(&progress, CLR_DIM "-------------------------------------");
				lines_add(&progress, notes);
			}
		}
	}
	else {
		int n = count_updates();
		lines_add(&progress, "");
		if (n) {
			char s[200];
			snprintf(s, sizeof s, "%d update(s):", n);
			lines_add(&progress, s);
			for (int i = 0; i < nports; i++)
				if (ports[i].update) {
					if (ports[i].asset_size >= 1024 * 1024)
						snprintf(s, sizeof s, "  %s: %s -> %s (%.1f MB)",
							ports[i].name, ports[i].version, ports[i].latest,
							(double)ports[i].asset_size / (1024.0 * 1024.0));
					else if (ports[i].asset_size > 0)
						snprintf(s, sizeof s, "  %s: %s -> %s (%ld KB)",
							ports[i].name, ports[i].version, ports[i].latest,
							(ports[i].asset_size + 1023) / 1024);
					else
						snprintf(s, sizeof s, "  %s: %s -> %s",
							ports[i].name, ports[i].version, ports[i].latest);
					lines_add(&progress, s);
				}
		} else {
			lines_add(&progress, "All installed ports are up to date.");
		}
		if (n && pm_self_update_available) {
			hints = "Up/Down  Start: update all  Sel: update PM  B: back";
			keys = key_both;
		} else if (n) {
			hints = "Up/Down  Start: update all  B: back";
			keys = key_start;
		} else if (pm_self_update_available) {
			hints = "Up/Down  Sel: update PM  B: back";
			keys = key_sel;
		}
	}
	return progress_end_at(hints, keys, 0);
}


static int confirm_choice(const char *question, const char *hints)
{
	header("Port Manager", NULL);
	Lines t = {0};
	lines_add(&t, question);
	for (int r = 0; r < t.n; r++)
		gfx_text(16, 180 + r * (CW + 6), t.l[r], COLS, C_TEXT);
	lines_free(&t);
	footer(hints);
	gfx_flip();
	for (;;) {
		int k = input_key(NULL);
		if (k == K_A)
			return 1;
		if (k == K_B)
			return 0;
		if (k < 0 || k == K_MENU)
			return -1;
	}
}

static int confirm(const char *question)
{
	return confirm_choice(question, "A: yes   B: no") == 1;
}

/* asks first unless already asked; on success restarts, with --check when the ports are to be
 * checked right after */
static void do_pm_self_update(int ask, int then_check)
{
	char q[300];
	snprintf(q, sizeof q, "Update Port Manager to %s?\n\n(Current version: %s)",
		pm_self_latest_version, pm_version());
	if (ask && !confirm(q))
		return;

	progress_begin("Updating Port Manager");
	say("--- updating Port Manager to %s", pm_self_latest_version);

	int err = pm_self_update();
	if (err) {
		lines_add(&progress, "");
		lines_add(&progress, CLR_BAD "Update failed:" CLR_TEXT);
		lines_add(&progress, pm_error);
		progress_end(NULL, NULL);
		return;
	}

	say("Port Manager updated successfully!");
	say(CLR_GOOD "Restarting..." CLR_TEXT);
	progress_draw("Restarting...");

	if (!g_self_bin[0])
		snprintf(g_self_bin, sizeof g_self_bin, "%s/portmanager", pm_here());

	progress_stop(); /* nothing may draw once the screen is closed */
	gfx_close();
	pm_unlock();
	/* the emulator can't run the downloaded (device) binary: the tests stop here */
	if (getenv("MIYOO_EMU_DIR")) {
		say("restart skipped (emulator)%s", then_check ? ", then --check" : "");
		exit(0);
	}
	char *restart_argv[] = {g_argv ? g_argv[0] : g_self_bin, then_check ? "--check" : NULL, NULL};
	execv(g_self_bin, restart_argv);
	exit(0);
}

static void do_update(Port *only);

/* Y on the list: Port Manager itself first, so a newer one checks and updates the ports */
static void check_all(int self_first)
{
	if (self_first) {
		progress_begin("Checking for updates");
		say("--- check Port Manager");
		say("Checking Port Manager...");
		int err = pm_check_self();
		progress_stop();
		if (err)
			snprintf(g_pm_line, sizeof g_pm_line, "Port Manager: can't check");
		else
			snprintf(g_pm_line, sizeof g_pm_line, "Port Manager: up to date (%s)", pm_version());
		if (!err && pm_self_update_available) {
			char q[300];
			snprintf(q, sizeof q, "Port Manager %s is available (installed: %s).\n\n"
				"It's updated first, then the ports are checked.", pm_self_latest_version, pm_version());
			int c = confirm_choice(q, "A: update now   B: later");
			if (c < 0)
				return;
			if (c == 1)
				do_pm_self_update(0, 1); /* returns only if it failed */
		}
	}
	else /* started again after updating itself */
		snprintf(g_pm_line, sizeof g_pm_line, "Port Manager: updated to %s", pm_version());
	int chk_res = do_check(NULL);
	if (chk_res == K_START)
		do_update(NULL);
	else if (chk_res == K_SELECT && pm_self_update_available)
		do_pm_self_update(1, 1);
}

static void do_update(Port *only)

{
	int n = only ? 1 : count_updates();
	char q[300];
	if (only && !only->installed)
		snprintf(q, sizeof q, "Install %s %s?", only->name, only->latest);
	else if (only && !*only->version)
		snprintf(q, sizeof q, "Install %s %s over the installed copy (version unknown)?\n\n"
			"Files it replaces and its saves are backed up first.", only->name, only->latest);
	else if (only)
		snprintf(q, sizeof q, "Update %s from %s to %s?\n\nIts saves are backed up first.", only->name, only->version, only->latest);
	else
		snprintf(q, sizeof q, "Update %d port(s)?\n\nTheir saves are backed up first.", n);
	if (!n || !confirm(q))
		return;
	progress_begin("Updating");
	say("--- update");
	int ok = 0, bad = 0, installing = only && !only->installed;
	for (int i = 0; i < nports; i++) {
		Port *p = &ports[i];
		if ((only && p != only) || (!only && !p->update))
			continue;
		if (n > 1)
			snprintf(progress_right, sizeof progress_right, "%d/%d", ok + bad + 1, n);
		port_begin(p);
		if (pm_update(p)) {
			port_failed();
			bad++;
		}
		else
			ok++;
		port_end();
	}
	char s[80];
	snprintf(s, sizeof s, "Done: %d %s%s", ok, installing ? "installed" : "updated", bad ? "" : ".");
	lines_add(&progress, "");
	lines_add(&progress, s);
	if (bad) {
		snprintf(s, sizeof s, "%d failed (see above).", bad);
		lines_add(&progress, s);
	}
	progress_end(NULL, NULL);
}

typedef struct {
	char label[128];
	char right[96];
	uint32_t right_clr;
} MenuItem;

/* a selected name too long for its row scrolls: a pause, one letter a tick, a pause, back */
#define MARQUEE_MS 200
static int marquee_tick;

static const char *marquee(const char *s, int max)
{
	int over = (int)strlen(s) - max, pause = 5;
	if (over <= 0)
		return s;
	int t = marquee_tick % (over + 2 * pause);
	return s + (t < pause ? 0 : t < pause + over ? t - pause : over);
}

/* waits for a key; while `scrolls`, gives the selected name its next tick (and returns 0) */
static int list_key(int scrolls)
{
	int k = input_key_wait(NULL, scrolls ? MARQUEE_MS : -1);
	if (k == 0)
		marquee_tick++;
	else
		marquee_tick = 0;
	return k;
}

/* also returns on `keys` (0-ended), with the key in *out_key */
static int choose_menu_keys(const char *title, const char *right_hdr, const MenuItem *items, int n, const char *hints, int initial_sel, const int *keys, int *out_key);

static int choose_menu_key(const char *title, const char *right_hdr, const MenuItem *items, int n, const char *hints, int initial_sel, int *out_key)
{
	static const int x[] = {K_X, 0};
	return choose_menu_keys(title, right_hdr, items, n, hints, initial_sel, out_key ? x : NULL, out_key);
}

static int choose_menu_keys(const char *title, const char *right_hdr, const MenuItem *items, int n, const char *hints, int initial_sel, const int *keys, int *out_key)
{
	if (n <= 0)
		return -1;
	int sel = initial_sel >= 0 && initial_sel < n ? initial_sel : 0;
	int top = 0;
	for (;;) {
		header(title, right_hdr);
		if (sel < top)
			top = sel;
		if (sel >= top + LIST_ROWS)
			top = sel - LIST_ROWS + 1;
		int scrolls = 0;
		for (int r = 0; r < LIST_ROWS && top + r < n; r++) {
			const MenuItem *item = &items[top + r];
			int y = LIST_Y + r * ROWH;
			int sl = item->right[0] ? (int)strlen(item->right) : 0;
			int max_len = COLS - sl - 2;
			if (max_len < 10) max_len = 10;
			const char *label = item->label;
			if (top + r == sel) {
				gfx_fill(8, y, SCR_W - 28, ROWH, C_SEL);
				scrolls = (int)strlen(label) > max_len;
				label = marquee(label, max_len);
			}
			gfx_text(16, y + (ROWH - CW) / 2, label, max_len, C_TEXT);
			if (sl > 0)
				gfx_text(SCR_W - 28 - sl * CW, y + (ROWH - CW) / 2, item->right, sl, item->right_clr);
		}
		scrollbar(top, LIST_ROWS, n, LIST_Y, LIST_ROWS * ROWH);
		footer(hints ? hints : "A: select   B: cancel");
		gfx_flip();

		int k = list_key(scrolls);
		if (k == 0)
			continue;
		if (k < 0 || k == K_B || k == K_MENU) {
			if (out_key) *out_key = k;
			return -1;
		}
		if (k == K_A || k == K_START) {
			if (out_key) *out_key = k;
			return sel;
		}
		for (const int *c = keys; out_key && c && *c; c++)
			if (k == *c) {
				*out_key = k;
				return sel;
			}
		if (k == K_UP && sel > 0) sel--;
		if (k == K_DOWN && sel < n - 1) sel++;
		if (k == K_L1 || k == K_LEFT) {
			sel -= LIST_ROWS - 1;
			if (sel < 0) sel = 0;
		}
		if (k == K_R1 || k == K_RIGHT) {
			sel += LIST_ROWS - 1;
			if (sel >= n) sel = n - 1;
		}
	}
}

static int choose_menu(const char *title, const char *right_hdr, const MenuItem *items, int n, const char *hints, int initial_sel)
{
	return choose_menu_key(title, right_hdr, items, n, hints, initial_sel, NULL);
}

static void alert_box(const char *msg)
{
	header("Port Manager", NULL);
	Lines t = {0};
	lines_add(&t, msg);
	for (int r = 0; r < t.n; r++)
		gfx_text(16, 180 + r * (CW + 6), t.l[r], COLS, C_TEXT);
	lines_free(&t);
	footer("B: back");
	gfx_flip();
	for (;;) {
		int k = input_key(NULL);
		if (k < 0 || k == K_B || k == K_A || k == K_MENU)
			break;
	}
}

static void size_text(char *s, size_t size, long long bytes)
{
	if (bytes >= 1024LL * 1024 * 1024)
		snprintf(s, size, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
	else if (bytes >= 1024 * 1024)
		snprintf(s, size, "%.1f MB", bytes / (1024.0 * 1024));
	else
		snprintf(s, size, "%lld KB", (bytes + 1023) / 1024);
}

/* one backup or all of them, each with the bar; the screen says what was freed */
static void compact_backups(const Backup *b, int n)
{
	progress_begin(n > 1 ? "Compacting backups" : "Compacting a backup");
	long long freed = 0;
	int bad = 0;
	for (int i = 0; i < n; i++) {
		if (n > 1)
			snprintf(progress_right, sizeof progress_right, "%d/%d", i + 1, n);
		item_begin(b[i].name);
		char s[200], sz[32];
		snprintf(s, sizeof s, CLR_DIM "before %s, %s", b[i].tag, b[i].date);
		lines_add(&progress, s);
		long long saved = 0;
		if (pm_backup_compact(&b[i], &saved)) {
			port_failed();
			bad++;
		}
		else {
			size_text(sz, sizeof sz, saved);
			snprintf(s, sizeof s, saved ? "%s freed" : "nothing to leave out", sz);
			lines_add(&progress, s);
			freed += saved;
		}
		port_end();
	}
	progress_right[0] = 0;
	char s[120], sz[32];
	size_text(sz, sizeof sz, freed);
	lines_add(&progress, "");
	snprintf(s, sizeof s, CLR_GOOD "Done: %s freed.", sz);
	lines_add(&progress, s);
	if (bad) {
		snprintf(s, sizeof s, CLR_BAD "%d failed (see above).", bad);
		lines_add(&progress, s);
	}
	progress_end(NULL, NULL);
}

/* Select on the main list: every backup (also of removed games) with what it takes; the player
 * compacts or deletes them, Port Manager never does on its own (owner, 2026-10-22) */
static void do_backups(void)
{
	static const int keys[] = {K_X, K_Y, 0};
	int sel = 0;
	for (;;) {
		Backup *bk = NULL;
		int n = 0;
		long long total = 0;
		pm_backups_all(&bk, &n, &total);
		if (!n) {
			free(bk);
			alert_box("No backups.\n\nBefore an update replaces a game's files, Port Manager keeps them and the saves in Backups/PortManager.");
			return;
		}
		MenuItem *items = calloc(n, sizeof *items);
		for (int i = 0; i < n; i++) {
			char sz[32];
			size_text(sz, sizeof sz, bk[i].bytes);
			/* made before updating to `tag`: it holds the older version */
			snprintf(items[i].label, sizeof items[i].label, "%s <%s", bk[i].name, bk[i].tag);
			snprintf(items[i].right, sizeof items[i].right, "%s", sz);
			items[i].right_clr = C_DIM;
		}
		char right[32];
		size_text(right, sizeof right, total);
		if (sel >= n)
			sel = n - 1;
		int key = 0;
		int s = choose_menu_keys("Backups", right, items, n, "A:compact X:delete Y:all B:back", sel, keys, &key);
		free(items);
		if (s < 0) {
			free(bk);
			return;
		}
		sel = s;
		char q[600], sz[32];
		size_text(sz, sizeof sz, bk[s].bytes);
		if (key == K_X) {
			snprintf(q, sizeof q, "Delete this backup?\n\n%s, before %s\n%s, %s\n\nIts saves and files will be gone for good.",
				bk[s].name, bk[s].tag, bk[s].date, sz);
			if (confirm_choice(q, "A: delete   B: cancel") == 1 && pm_backup_delete(&bk[s]))
				alert_box(pm_error);
		}
		else if (key == K_Y) {
			size_text(sz, sizeof sz, total);
			snprintf(q, sizeof q, "Compact all %d backups?\n%s now\n\nLeaves out logs and data the games build again from their ROMs. Saves stay.", n, sz);
			if (confirm(q))
				compact_backups(bk, n);
		}
		else {
			snprintf(q, sizeof q, "Compact this backup?\n\n%s, before %s\n%s, %s\n\nLeaves out logs and data the game builds again from its ROM. Saves stay.",
				bk[s].name, bk[s].tag, bk[s].date, sz);
			if (confirm(q))
				compact_backups(&bk[s], 1);
		}
		free(bk);
	}
}

static void do_restore(Port *p)
{
	MenuItem options[2] = {
		{"Restore a backup", "Backups/", C_DIM},
		{"Install an older version", "GitHub", C_DIM}
	};
	int choice = choose_menu("Restore / Rollback", p->name, options, 2, "A: select   B: cancel", 0);
	if (choice < 0)
		return;

	if (choice == 0) {
		/* 1. Local backup */
		int sel_idx = 0;
		for (;;) {
			BackupItem *backups = NULL;
			int nb = 0;
			if (pm_list_backups(p, &backups, &nb) != 0 || nb == 0) {
				char msg[300];
				snprintf(msg, sizeof msg, "No backups found for %s\nin Backups/PortManager/.", p->name);
				alert_box(msg);
				pm_free_backups(backups, nb);
				return;
			}

			if (sel_idx >= nb)
				sel_idx = nb - 1;

			MenuItem *items = malloc(nb * sizeof *items);
			for (int i = 0; i < nb; i++) {
				snprintf(items[i].label, sizeof items[i].label, "%s", backups[i].date);
				if (backups[i].size >= 1024 * 1024)
					snprintf(items[i].right, sizeof items[i].right, "%s (%.1f MB)",
						*backups[i].tag ? backups[i].tag : "bak", (double)backups[i].size / (1024.0 * 1024.0));
				else if (backups[i].size > 0)
					snprintf(items[i].right, sizeof items[i].right, "%s (%ld KB)",
						*backups[i].tag ? backups[i].tag : "bak", (backups[i].size + 1023) / 1024);
				else
					snprintf(items[i].right, sizeof items[i].right, "%s (folder)",
						*backups[i].tag ? backups[i].tag : "bak");
				items[i].right_clr = C_ACCENT;
			}

			int key_pressed = 0;
			int b_sel = choose_menu_key("Local backups", p->name, items, nb, "A: restore   X: delete   B: cancel", sel_idx, &key_pressed);
			free(items);
			if (b_sel < 0) {
				pm_free_backups(backups, nb);
				return;
			}
			sel_idx = b_sel;

			if (key_pressed == K_X) {
				char q[512];
				snprintf(q, sizeof q, "Delete backup from %s?\n\nThis will permanently delete this backup.",
					backups[b_sel].display);
				if (confirm_choice(q, "A: delete   B: cancel") == 1) {
					remove_tree(backups[b_sel].path);
				}
				pm_free_backups(backups, nb);
				continue;
			}

			if (key_pressed == K_A || key_pressed == K_START) {
				char q[512];
				snprintf(q, sizeof q, "Restore backup from %s?\n\nThis will restore previous binaries and saved game data.",
					backups[b_sel].display);
				if (confirm(q)) {
					progress_begin("Restoring backup");
					say("--- restore %s", p->id);
					if (pm_restore_backup(p, &backups[b_sel])) {
						lines_add(&progress, "");
						lines_add(&progress, "Can't restore backup:");
						lines_add(&progress, pm_error);
					} else {
						lines_add(&progress, "");
						lines_add(&progress, "Restore completed successfully.");
					}
					progress_end(NULL, NULL);
				}
				pm_free_backups(backups, nb);
				return;
			}

			pm_free_backups(backups, nb);
			return;
		}
	} else if (choice == 1) {
		/* 2. Older release from GitHub */
		progress_begin("GitHub releases");
		say("--- fetch releases %s", p->repo);
		RemoteRelease *releases = NULL;
		int nr = 0;
		if (pm_list_remote_releases(p, &releases, &nr) != 0 || nr == 0) {
			lines_add(&progress, "");
			lines_add(&progress, "Can't fetch releases:");
			lines_add(&progress, pm_error[0] ? pm_error : "No releases found");
			progress_end(NULL, NULL);
			pm_free_remote_releases(releases, nr);
			return;
		}
		lines_free(&progress);
		log_sink = NULL;

		MenuItem *items = malloc(nr * sizeof *items);
		for (int i = 0; i < nr; i++) {
			snprintf(items[i].label, sizeof items[i].label, "%s", releases[i].tag);
			if (p->installed && *p->version && !strcmp(p->version, releases[i].tag)) {
				snprintf(items[i].right, sizeof items[i].right, "[installed]");
				items[i].right_clr = C_GOOD;
			} else if (releases[i].asset_size >= 1024 * 1024) {
				snprintf(items[i].right, sizeof items[i].right, "%.1f MB", (double)releases[i].asset_size / (1024.0 * 1024.0));
				items[i].right_clr = C_DIM;
			} else if (releases[i].asset_size > 0) {
				snprintf(items[i].right, sizeof items[i].right, "%ld KB", (releases[i].asset_size + 1023) / 1024);
				items[i].right_clr = C_DIM;
			} else {
				items[i].right[0] = 0;
				items[i].right_clr = C_DIM;
			}
		}

		int r_sel = choose_menu("GitHub releases", p->name, items, nr, "A: install   B: cancel", 0);
		free(items);
		if (r_sel >= 0) {
			char q[512];
			snprintf(q, sizeof q, "Install %s release %s?\n\nBefore installing, current files will be backed up into Backups/PortManager/.",
				p->name, releases[r_sel].tag);
			if (confirm(q)) {
				progress_begin("Installing release");
				say("--- install %s -> %s", p->id, releases[r_sel].tag);
				port_begin(p);
				if (pm_install_release(p, &releases[r_sel]))
					port_failed();
				port_end();
				lines_add(&progress, "");
				lines_add(&progress, pm_error[0] ? "Can't install release." : "Installation completed.");
				progress_end(NULL, NULL);
			}
		}
		pm_free_remote_releases(releases, nr);
	}
}

static void details(Port *p)
{
	static char buf[8192];
	for (;;) {
		pm_info(p, buf, sizeof buf);
		Lines t = {0};
		lines_add(&t, buf);
		/* A performs the primary action: check if unchecked/up to date, or update/install;
		 * Y opens restore/rollback; X removes */
		int unknown = p->installed && p->checked && !*p->version;
		int installable = !p->installed && p->checked && *p->latest;
		int has_action = (p->update || unknown || installable);
		int playable = p->installed && !*p->missing;
		int keys[6] = {0}, nk = 0;
		keys[nk++] = K_SELECT;
		if (*p->asset || has_action)
			keys[nk++] = K_A;
		if (p->installed || *p->asset)
			keys[nk++] = K_Y;
		if (p->installed)
			keys[nk++] = K_X;
		if (playable)
			keys[nk++] = K_START;
		const char *hints =
			p->installed && p->update ? "A:update  Y:restore  X:delete  B:back" :
			p->installed && unknown   ? "A:install  Y:restore  X:delete  B:back" :
			installable               ? "A:install  Y:restore  B:back" :
			p->installed              ? "A:check  Y:restore  X:delete  B:back" :
			*p->asset                 ? "A:check  Y:restore  B:back" : "B:back";
		int k = view(p->name, p->category, &t, hints, keys);
		lines_free(&t);
		if (k == K_A) {
			if (has_action)
				do_update(p);
			else if (*p->asset) {
				if (do_check(p) == K_A)
					do_update(p);
			}
		}
		else if (k == K_SELECT) {
			static char more[8192];
			pm_details(p, more, sizeof more);
			Lines m = {0};
			lines_add(&m, more);
			view(p->name, "info", &m, "Up/Down   B: back", NULL);
			lines_free(&m);
		}
		else if (k == K_Y && (p->installed || *p->asset)) {
			do_restore(p);
		}
		else if (k == K_START && playable) {
			if (pm_launch(p) == 0) {
				g_launch = 1; /* Port Manager closes and Onion starts the game */
				return;
			}
			Lines e = {0};
			lines_add(&e, CLR_BAD "Can't start the game." CLR_TEXT);
			lines_add(&e, pm_error);
			view(p->name, p->category, &e, "B: back", NULL);
			lines_free(&e);
		}
		else if (k == K_X && p->installed) {
			char q1[256];
			snprintf(q1, sizeof q1, "Remove %s?\n\nThis will remove the port and its shortcut.", p->name);
			if (!confirm(q1))
				continue;
			char q2[512];
			snprintf(q2, sizeof q2, "Keep game data in the folder?\n(%s)\n\ne.g. ROMs, saves, game data files.", p->dir);
			header("Port Manager", NULL);
			Lines t = {0};
			lines_add(&t, q2);
			for (int r = 0; r < t.n; r++)
				gfx_text(16, 180 + r * (CW + 6), t.l[r], COLS, C_TEXT);
			lines_free(&t);
			footer("A: keep data   Y: delete all   B: cancel");
			gfx_flip();
			int choice = -1;
			for (;;) {
				int chk = input_key(NULL);
				if (chk == K_A) {
					choice = 1;
					break;
				}
				if (chk == K_Y) {
					choice = 0;
					break;
				}
				if (chk == K_B || chk == K_MENU || chk < 0) {
					choice = -1;
					break;
				}
			}
			if (choice < 0)
				continue;
			if (choice == 0) {
				char q3[256];
				snprintf(q3, sizeof q3, "Are you SURE you want to delete ALL files in %s including ROMs and saves?", p->name);
				if (!confirm(q3))
					continue;
			}
			int keep_data = (choice == 1);
			progress_begin("Removing port");
			say("--- remove %s", p->id);
			char dir[600];
			snprintf(dir, sizeof dir, "%s", p->dir); /* the scan after removing may move ports */
			port_begin(p);
			if (pm_uninstall(p, keep_data))
				port_failed();
			port_end();
			lines_add(&progress, "");
			if (pm_error[0])
				lines_add(&progress, CLR_BAD "Can't remove the port.");
			else if (pm_folder_kept) {
				lines_add(&progress, CLR_GOOD "Removed. Your game files stay in:");
				lines_add(&progress, dir);
			} else
				lines_add(&progress, CLR_GOOD "Removed, with its game folder.");
			progress_end(NULL, NULL);
			return;
		}
		else
			return;
	}
}

/* --- lists --- */

typedef struct {
	int port; /* -1: a category heading */
	const char *heading;
} Row;

static int by_name(const void *a, const void *b)
{
	return strcasecmp(ports[*(const int *)a].name, ports[*(const int *)b].name);
}

static int by_category(const void *a, const void *b)
{
	const Port *p = &ports[*(const int *)a], *q = &ports[*(const int *)b];
	int c = strcasecmp(p->category, q->category);
	return c ? c : strcasecmp(p->name, q->name);
}

/* the right-hand status of a port's row, and its colour */
static const char *status(const Port *p, int all, uint32_t *c)
{
	static char s[160];
	*c = C_DIM;
	if (!p->installed) {
		snprintf(s, sizeof s, *p->asset ? "not installed" : "no release");
		return s;
	}
	if (p->update) {
		*c = C_ACCENT;
		snprintf(s, sizeof s, "%s>%s", p->version, p->latest);
		if (strlen(s) > 16)
			snprintf(s, sizeof s, "update");
	}
	else if (*p->missing) {
		*c = C_WARN;
		snprintf(s, sizeof s, "files missing");
	}
	else
		snprintf(s, sizeof s, "%s", *p->version ? p->version : (all ? "installed" : "?"));
	return s;
}

/* All ports: A-Z, or by category after Select (kept until Port Manager closes) */
static int g_by_category;

/* all = the whole catalog; else the installed ports by name */
static int run_list(int all)
{
	int sel = 0, top = 0, keep = -1;
	for (;;) {
		int grouped = all && g_by_category;
		if (!all && g_check_at_start) {
			int self_first = g_check_at_start == 2;
			g_check_at_start = 0;
			check_all(self_first);
		}
		int idx[256], n = 0;
		for (int i = 0; i < nports && n < 256; i++)
			if (all || ports[i].installed)
				idx[n++] = i;
		qsort(idx, n, sizeof *idx, grouped ? by_category : by_name);
		Row rows[512];
		int nr = 0;
		for (int i = 0; i < n; i++) {
			if (grouped && (i == 0 || strcasecmp(ports[idx[i]].category, ports[idx[i - 1]].category)))
				rows[nr++] = (Row){-1, ports[idx[i]].category};
			rows[nr++] = (Row){idx[i], NULL};
		}
		/* after switching the order, stay on the same port */
		for (int r = 0; keep >= 0 && r < nr; r++)
			if (rows[r].port == keep)
				sel = r;
		keep = -1;
		while (sel < nr && rows[sel].port < 0)
			sel++;
		if (sel >= nr)
			sel = 0;

		int nup = count_updates();
		char right[64];
		if (!all && pm_self_update_available)
			snprintf(right, sizeof right, "%d inst [Update!]", n);
		else
			snprintf(right, sizeof right, all ? "%d ports" : "%d installed", n);
		header(all ? "All ports" : "Port Manager", right);
		int scrolls = 0;
		if (sel < top)
			top = sel;
		if (sel >= top + LIST_ROWS)
			top = sel - LIST_ROWS + 1;
		if (top > 0 && rows[top - 1].port < 0 && sel - (top - 1) < LIST_ROWS)
			top--; /* keep a heading with its first port */
		if (nr == 0 && !all) {
			/* a new player's first screen: how to get games, X lit in the footer too */
			static const char *const help[] = {
				"No ports installed yet.",
				"",
				"How to get games:",
				"1. Press " CLR_GOOD "X" CLR_TEXT ": all ports to install.",
				"2. Choose a game and press " CLR_GOOD "A" CLR_TEXT ",",
				"   then " CLR_GOOD "A" CLR_TEXT " to check and to install it.",
				"3. If it needs files from your own",
				"   copy of the game (ROM, data),",
				"   its page says which and where.",
			};
			for (int r = 0; r < (int)(sizeof help / sizeof *help); r++)
				gfx_text(16, LIST_Y + 8 + r * ROWH, help[r], COLS, r ? C_TEXT : C_ACCENT);
		} else {
			for (int r = 0; r < LIST_ROWS && top + r < nr; r++) {
				const Row *row = &rows[top + r];
				int y = LIST_Y + r * ROWH;
				if (row->port < 0) {
					gfx_text(16, y + (ROWH - CW) / 2, row->heading, COLS, C_ACCENT);
					continue;
				}
				const Port *p = &ports[row->port];
				uint32_t c;
				const char *s = status(p, all, &c);
				int sl = strlen(s);
				int max_len = COLS - sl - 2 - (grouped ? 1 : 0);
				const char *name = p->name;
				if (top + r == sel) {
					gfx_fill(8, y, SCR_W - 28, ROWH, C_SEL);
					scrolls = (int)strlen(name) > max_len;
					name = marquee(name, max_len);
				}
				gfx_text(grouped ? 32 : 16, y + (ROWH - CW) / 2, name, max_len, C_TEXT);
				gfx_text(SCR_W - 28 - sl * CW, y + (ROWH - CW) / 2, s, sl, c);
			}
			scrollbar(top, LIST_ROWS, nr, LIST_Y, LIST_ROWS * ROWH);
		}
		if (all)
			footer(grouped ? "A: details  Sel: A-Z  B: back" : "A: details  Sel: categories  B: back");
		else if (nr == 0)
			footer(CLR_GOOD "X: get ports" CLR_DIM "   Y: check   B: exit");
		else /* Port Manager's own update comes first under Y; B exits everywhere */
			footer(nup ? "A:info X:all Y:chk St:upd Sel:backups" : "A:info X:all Y:check Sel:backups");
		gfx_flip();

		int k = list_key(scrolls);
		if (k == 0)
			continue;
		if (k < 0 || k == K_B || k == K_MENU)
			return 0;
		int step = 0;
		if (k == K_UP) step = -1;
		if (k == K_DOWN) step = 1;
		if (k == K_L1 || k == K_LEFT) step = -(LIST_ROWS - 1);
		if (k == K_R1 || k == K_RIGHT) step = LIST_ROWS - 1;
		if (step) {
			int s = sel + step;
			if (s < 0) s = 0;
			if (s >= nr) s = nr - 1;
			/* headings can't be chosen */
			while (s > 0 && s < nr && rows[s].port < 0)
				s += step > 0 ? 1 : -1;
			if (s >= 0 && s < nr && rows[s].port >= 0)
				sel = s;
		}
		if (k == K_A && nr && rows[sel].port >= 0)
			details(&ports[rows[sel].port]);
		if (g_launch)
			return 0;
		if (all && k == K_SELECT) {
			keep = nr ? rows[sel].port : -1;
			g_by_category = !g_by_category;
			top = 0;
		}
		if (!all && k == K_X)
			run_list(1);
		if (g_launch)
			return 0;
		if (!all && k == K_Y)
			check_all(1);
		if (!all && k == K_SELECT)
			do_backups();
		if (!all && k == K_START && nup)
			do_update(NULL);
	}
}

int main(int argc, char **argv)
{
	g_argv = argv;
	g_check_at_start = argc > 1 && !strcmp(argv[1], "--check");
	ssize_t n = readlink("/proc/self/exe", g_self_bin, sizeof(g_self_bin) - 1);
	if (n > 0)
		g_self_bin[n] = 0;
	if (gfx_open()) {

		fprintf(stderr, "can't open the screen or the buttons\n");
		return 1;
	}
	int r = 0;
	if (pm_init()) {
		Lines t = {0};
		lines_add(&t, "The catalog (catalog.json next to this program) is missing or broken.");
		view("Port Manager", NULL, &t, "B: exit", 0);
		lines_free(&t);
		r = 1;
	}
	else if (pm_lock()) {
		Lines t = {0};
		lines_add(&t, "The Port Manager is already running.");
		view("Port Manager", NULL, &t, "B: exit", 0);
		lines_free(&t);
		r = 1;
	}
	else {
		if (pm_no_catalog && !g_check_at_start)
			g_check_at_start = 2;
		run_list(0);
		pm_unlock();
	}
	gfx_close();
	return r;
}
