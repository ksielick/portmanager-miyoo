/* pmview: a scrollable text page for the Port manager on OnionOS (Miyoo Mini).
 *   pmview <title> <file>
 * Up/Down: a line, Left/Right or L1/R1: a page, A, B or Menu: close.
 * Draws straight to /dev/fb0, on the page Onion shows (the panel is upside down, so the
 * picture is turned 180 degrees), and reads the buttons from /dev/input/event0. Text is
 * wrapped at word boundaries; anything outside ASCII shows as '?'.
 * Font: font8x8_basic (Daniel Hepper, public domain), drawn at twice its size. */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "font8x8_basic.h"

#define W 640
#define H 480
#define SCALE 2
#define CW (8 * SCALE)             /* character cell */
#define LH (CW + 6)                /* line height */
#define MX 16                      /* left/right margin */
#define TOP 48                     /* title bar */
#define BOTTOM 40                  /* footer */
#define COLS ((W - 2 * MX - 12) / CW)
#define ROWS ((H - TOP - BOTTOM - 16) / LH)

/* the Miyoo's evdev codes (A, B, Menu, L1, R1) */
#define BTN_KEY_A KEY_SPACE
#define BTN_KEY_B KEY_LEFTCTRL
#define BTN_KEY_MENU KEY_ESC
#define BTN_KEY_L1 KEY_E
#define BTN_KEY_R1 KEY_T

#define C_BG 0x22252B
#define C_BAR 0x2E323A
#define C_TEXT 0xF0F0F0
#define C_DIM 0x9AA0AA
#define C_THUMB 0x7B6CD9

static uint32_t screen[W * H];
static char **lines;
static int nlines, cap;

static void add_line(const char *s, size_t n)
{
	if (nlines == cap) {
		cap = cap ? cap * 2 : 64;
		lines = realloc(lines, cap * sizeof *lines);
		if (!lines)
			exit(1);
	}
	lines[nlines++] = strndup(s, n);
}

/* UTF-8 sequences become one '?', CR and tabs go */
static void to_ascii(char *s)
{
	char *d = s;
	for (; *s; s++) {
		unsigned char c = *s;
		if (c == '\r')
			continue;
		if (c == '\t')
			*d++ = ' ';
		else if (c < 0x80)
			*d++ = c;
		else if (c >= 0xC0)
			*d++ = '?';
	}
	*d = 0;
}

static void wrap(const char *text)
{
	const char *p = text;
	for (;;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p), i = 0;
		if (len == 0)
			add_line("", 0);
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
				add_line(p + start, len - start);
				break;
			}
			if (p[j] == ' ')
				last = j;
			size_t end = last > start ? last : j; /* a word longer than a line is cut */
			add_line(p + start, end - start);
			i = end;
		}
		if (!e)
			break;
		p = e + 1;
	}
	/* no empty lines at the end */
	while (nlines > 1 && lines[nlines - 1][0] == 0)
		free(lines[--nlines]);
}

static void fill(int x, int y, int w, int h, uint32_t c)
{
	for (int yy = y; yy < y + h && yy < H; yy++)
		for (int xx = x; xx < x + w && xx < W; xx++)
			screen[yy * W + xx] = c;
}

static void text(int x, int y, const char *s, int max, uint32_t c)
{
	for (int n = 0; s[n] && n < max; n++, x += CW) {
		const char *g = font8x8_basic[(unsigned char)s[n] & 127];
		for (int yy = 0; yy < 8; yy++)
			for (int xx = 0; xx < 8; xx++)
				if (g[yy] >> xx & 1)
					fill(x + xx * SCALE, y + yy * SCALE, SCALE, SCALE, c);
	}
}

static void render(const char *title, int top)
{
	char s[64];
	int tl = strlen(title) < COLS ? (int)strlen(title) : COLS;

	fill(0, 0, W, H, C_BG);
	fill(0, 0, W, TOP, C_BAR);
	text((W - tl * CW) / 2, (TOP - CW) / 2, title, tl, C_TEXT);
	for (int r = 0; r < ROWS && top + r < nlines; r++)
		text(MX, TOP + 8 + r * LH, lines[top + r], COLS, C_TEXT);
	if (nlines > ROWS) {
		int track = H - TOP - BOTTOM - 16, y0 = TOP + 8;
		int th = track * ROWS / nlines, ty = y0 + (track - th) * top / (nlines - ROWS);
		fill(W - 10, y0, 4, track, C_BAR);
		fill(W - 12, ty, 8, th < 16 ? 16 : th, C_THUMB);
		snprintf(s, sizeof s, "%d-%d/%d", top + 1, top + ROWS, nlines);
		text(W - MX - (int)strlen(s) * CW, H - BOTTOM + (BOTTOM - CW) / 2, s, 20, C_DIM);
		text(MX, H - BOTTOM + (BOTTOM - CW) / 2, "Up/Down  B: back", COLS, C_DIM);
	}
	else
		text(MX, H - BOTTOM + (BOTTOM - CW) / 2, "B: back", COLS, C_DIM);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: pmview <title> <file>\n");
		return 2;
	}

	FILE *f = fopen(argv[2], "rb");
	if (!f) {
		perror(argv[2]);
		return 1;
	}
	static char data[65536];
	size_t n = fread(data, 1, sizeof data - 1, f);
	fclose(f);
	data[n] = 0;
	to_ascii(data);
	wrap(data);

	int fb = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) {
		perror("/dev/fb0");
		return 1;
	}
	uint8_t *mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	int in = open("/dev/input/event0", O_RDONLY);
	if (in < 0) {
		perror("/dev/input/event0");
		return 1;
	}

	int top = 0, last = nlines > ROWS ? nlines - ROWS : 0;
	struct input_event ev;
	for (;;) {
		/* the shown page, turned 180 degrees */
		render(argv[1], top);
		uint32_t *page = (uint32_t *)(mem + var.yoffset * fix.line_length);
		int stride = fix.line_length / 4;
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
				page[(H - 1 - y) * stride + (W - 1 - x)] = 0xFF000000 | screen[y * W + x];

		int old = top;
		while (top == old) {
			if (read(in, &ev, sizeof ev) != sizeof ev)
				return 0;
			if (ev.type != EV_KEY || ev.value == 0)
				continue;
			switch (ev.code) {
			case KEY_UP: top--; break;
			case KEY_DOWN: top++; break;
			case KEY_LEFT: case BTN_KEY_L1: top -= ROWS - 1; break;
			case KEY_RIGHT: case BTN_KEY_R1: top += ROWS - 1; break;
			case BTN_KEY_A: case BTN_KEY_B: case BTN_KEY_MENU:
				if (ev.value == 1)
					return 0;
				break;
			}
			if (top > last)
				top = last;
			if (top < 0)
				top = 0;
		}
	}
}
