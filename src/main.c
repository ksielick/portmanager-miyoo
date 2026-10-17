/* Port manager for OnionOS on the Miyoo Mini (portmaster-miyoo): lists the installed ports
 * and the whole catalog, shows a port's details and how to add its game files, checks the
 * authors' GitHub releases and installs updates. https://github.com/ksielick/portmaster-miyoo
 * Buttons: Up/Down choose, L1/R1 a page, A details, X all ports, Y check for updates,
 * Start update all, B back (exit on the first screen). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gfx.h"
#include "pm.h"
#include "util.h"

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
	gfx_text(16, (TOPH - CW) / 2, title, COLS, C_TEXT);
	if (right && *right)
		gfx_text(SCR_W - 16 - (int)strlen(right) * CW, (TOPH - CW) / 2, right, 12, C_DIM);
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
		while (i < len) {
			while (i < len && p[i] == ' ')
				i++;
			if (i >= len)
				break;
			size_t start = i, last = 0, j = i;
			while (j < len && j - start < COLS) {
				if (p[j] == ' ')
					last = j;
				j++;
			}
			if (j >= len) {
				lines_push(t, p + start, len - start);
				break;
			}
			if (p[j] == ' ')
				last = j;
			size_t end = last > start ? last : j;
			lines_push(t, p + start, end - start);
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

static void draw_lines(const Lines *t, int top)
{
	for (int r = 0; r < TEXT_ROWS && top + r < t->n; r++)
		gfx_text(16, TOPH + 8 + r * (CW + 6), t->l[top + r], COLS, C_TEXT);
	scrollbar(top, TEXT_ROWS, t->n, TOPH + 8, TEXT_ROWS * (CW + 6));
}

/* a scrollable page; returns the key that closed it (B, or one of `keys`, 0-terminated) */
static int view(const char *title, const Lines *t, const char *hints, const int *keys)
{
	int top = 0, last = t->n > TEXT_ROWS ? t->n - TEXT_ROWS : 0;
	for (;;) {
		header(title, NULL);
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

/* --- the progress page: pm's log lines appear as they come --- */

static Lines progress;
static const char *progress_title;

static void progress_draw(const char *hints)
{
	int top = progress.n > TEXT_ROWS ? progress.n - TEXT_ROWS : 0;
	header(progress_title, NULL);
	draw_lines(&progress, top);
	footer(hints);
	gfx_flip();
}

static void progress_sink(const char *line)
{
	/* lines meant for the log only */
	if (!strncmp(line, "NONET ", 6) || !strncmp(line, "ERROR ", 6) || !strncmp(line, "--- ", 4))
		return;
	lines_add(&progress, line);
	progress_draw("Please wait...");
}

static void progress_begin(const char *title)
{
	lines_free(&progress);
	progress_title = title;
	log_sink = progress_sink;
	progress_draw("Please wait...");
}

static void progress_end(void)
{
	log_sink = NULL;
	view(progress_title, &progress, "Up/Down  B: back", NULL);
	lines_free(&progress);
}

/* --- actions --- */

static int count_updates(void)
{
	int n = 0;
	for (int i = 0; i < nports; i++)
		n += ports[i].update;
	return n;
}

/* every installed port, or only one (from its details) */
static void do_check(const Port *only)
{
	progress_begin("Checking for updates");
	say(only ? "--- check %s" : "--- check", only ? only->id : "");
	if (pm_check(only)) {
		lines_add(&progress, "");
		lines_add(&progress, "Can't check for updates:");
		lines_add(&progress, pm_error);
	}
	else if (only) {
		char s[300];
		if (only->update)
			snprintf(s, sizeof s, "%s: %s -> %s\n\nB: back, then A: update.", only->name, only->version, only->latest);
		else if (only->installed)
			snprintf(s, sizeof s, "%s is up to date (%s).", only->name, only->latest);
		else
			snprintf(s, sizeof s, "%s: the latest release is %s.", only->name, only->latest);
		lines_add(&progress, "");
		lines_add(&progress, s);
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
					snprintf(s, sizeof s, "  %s: %s -> %s", ports[i].name, ports[i].version, ports[i].latest);
					lines_add(&progress, s);
				}
			lines_add(&progress, "");
			lines_add(&progress, "Start on the list: update all.");
		}
		else
			lines_add(&progress, "All installed ports are up to date.");
	}
	progress_end();
}

static int confirm(const char *question)
{
	header("Port manager", NULL);
	Lines t = {0};
	lines_add(&t, question);
	for (int r = 0; r < t.n; r++)
		gfx_text(16, 180 + r * (CW + 6), t.l[r], COLS, C_TEXT);
	lines_free(&t);
	footer("A: yes   B: no");
	gfx_flip();
	for (;;) {
		int k = input_key(NULL);
		if (k == K_A)
			return 1;
		if (k < 0 || k == K_B || k == K_MENU)
			return 0;
	}
}

static void do_update(Port *only)
{
	int n = only ? 1 : count_updates();
	char q[300];
	if (only)
		snprintf(q, sizeof q, "Update %s from %s to %s?\n\nIts saves are backed up first.", only->name, only->version, only->latest);
	else
		snprintf(q, sizeof q, "Update %d port(s)?\n\nTheir saves are backed up first.", n);
	if (!n || !confirm(q))
		return;
	progress_begin("Updating");
	say("--- update");
	int ok = 0, bad = 0;
	for (int i = 0; i < nports; i++) {
		Port *p = &ports[i];
		if ((only && p != only) || (!only && !p->update))
			continue;
		if (pm_update(p)) {
			lines_add(&progress, pm_error);
			bad++;
		}
		else
			ok++;
	}
	char s[80];
	snprintf(s, sizeof s, "Done: %d updated%s", ok, bad ? "" : ".");
	lines_add(&progress, "");
	lines_add(&progress, s);
	if (bad) {
		snprintf(s, sizeof s, "%d failed (see above).", bad);
		lines_add(&progress, s);
	}
	progress_end();
}

static void details(Port *p)
{
	static char buf[8192];
	for (;;) {
		pm_info(p, buf, sizeof buf);
		Lines t = {0};
		lines_add(&t, buf);
		/* Y checks this port's repo; A installs its update once one is known */
		int keys[3] = {0}, nk = 0;
		if (*p->asset)
			keys[nk++] = K_Y;
		if (p->update)
			keys[nk++] = K_A;
		const char *hints = p->update ? "Up/Down A:update Y:check B:back" :
			*p->asset ? "Up/Down  Y: check  B: back" : "Up/Down  B: back";
		int k = view(p->name, &t, hints, keys);
		lines_free(&t);
		if (k == K_Y)
			do_check(p);
		else if (k == K_A)
			do_update(p);
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
	}
	else if (*p->missing) {
		*c = C_WARN;
		snprintf(s, sizeof s, "files missing");
	}
	else
		snprintf(s, sizeof s, "%s", *p->version ? p->version : (all ? "installed" : "?"));
	return s;
}

/* all = the whole catalog by category; else the installed ports by name */
static int run_list(int all)
{
	int sel = 0, top = 0;
	for (;;) {
		int idx[256], n = 0;
		for (int i = 0; i < nports && n < 256; i++)
			if (all || ports[i].installed)
				idx[n++] = i;
		qsort(idx, n, sizeof *idx, all ? by_category : by_name);
		Row rows[512];
		int nr = 0;
		for (int i = 0; i < n; i++) {
			if (all && (i == 0 || strcasecmp(ports[idx[i]].category, ports[idx[i - 1]].category)))
				rows[nr++] = (Row){-1, ports[idx[i]].category};
			rows[nr++] = (Row){idx[i], NULL};
		}
		while (sel < nr && rows[sel].port < 0)
			sel++;
		if (sel >= nr)
			sel = 0;

		int nup = count_updates();
		char right[32];
		snprintf(right, sizeof right, all ? "%d ports" : "%d installed", n);
		header(all ? "All ports" : "Port manager", right);
		if (sel < top)
			top = sel;
		if (sel >= top + LIST_ROWS)
			top = sel - LIST_ROWS + 1;
		if (top > 0 && rows[top - 1].port < 0 && sel - (top - 1) < LIST_ROWS)
			top--; /* keep a heading with its first port */
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
			if (top + r == sel)
				gfx_fill(8, y, SCR_W - 28, ROWH, C_SEL);
			gfx_text(all ? 32 : 16, y + (ROWH - CW) / 2, p->name, COLS - sl - 2 - (all ? 1 : 0), C_TEXT);
			gfx_text(SCR_W - 28 - sl * CW, y + (ROWH - CW) / 2, s, sl, c);
		}
		scrollbar(top, LIST_ROWS, nr, LIST_Y, LIST_ROWS * ROWH);
		if (all)
			footer("A: details  B: back");
		else
			footer(nup ? "A:Info X:All Y:Check St:Update B:Exit" : "A: info  X: all  Y: check  B: exit");
		gfx_flip();

		int k = input_key(NULL);
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
		if (!all && k == K_X)
			run_list(1);
		if (!all && k == K_Y)
			do_check(NULL);
		if (!all && k == K_START && nup)
			do_update(NULL);
	}
}

int main(void)
{
	if (gfx_open()) {
		fprintf(stderr, "can't open the screen or the buttons\n");
		return 1;
	}
	int r = 0;
	if (pm_init()) {
		Lines t = {0};
		lines_add(&t, "The catalog (catalog.json next to this program) is missing or broken.");
		view("Port manager", &t, "B: exit", 0);
		lines_free(&t);
		r = 1;
	}
	else if (pm_lock()) {
		Lines t = {0};
		lines_add(&t, "The port manager is already running.");
		view("Port manager", &t, "B: exit", 0);
		lines_free(&t);
		r = 1;
	}
	else {
		run_list(0);
		pm_unlock();
	}
	gfx_close();
	return r;
}
