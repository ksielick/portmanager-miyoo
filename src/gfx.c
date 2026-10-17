#include "gfx.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vendor/font8x8_basic.h"

static uint32_t screen[SCR_W * SCR_H];
static int fb = -1, in = -1;
static uint8_t *mem;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;

int gfx_open(void)
{
	fb = open("/dev/fb0", O_RDWR);
	if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix))
		return -1;
	mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (mem == MAP_FAILED)
		return -1;
	in = open("/dev/input/event0", O_RDONLY);
	return in < 0 ? -1 : 0;
}

void gfx_close(void)
{
	/* a black screen until MainUI draws again */
	gfx_fill(0, 0, SCR_W, SCR_H, 0);
	gfx_flip();
	if (mem && mem != MAP_FAILED)
		munmap(mem, fix.smem_len);
	if (fb >= 0)
		close(fb);
	if (in >= 0)
		close(in);
}

void gfx_fill(int x, int y, int w, int h, uint32_t c)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > SCR_W) w = SCR_W - x;
	if (y + h > SCR_H) h = SCR_H - y;
	for (int yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++)
			screen[yy * SCR_W + xx] = c;
}

int gfx_text(int x, int y, const char *s, int max, uint32_t c)
{
	for (int n = 0; s[n] && n < max; n++, x += CW) {
		const char *g = font8x8_basic[(unsigned char)s[n] & 127];
		for (int yy = 0; yy < 8; yy++)
			for (int xx = 0; xx < 8; xx++)
				if (g[yy] >> xx & 1)
					gfx_fill(x + xx * FONT_SCALE, y + yy * FONT_SCALE, FONT_SCALE, FONT_SCALE, c);
	}
	return x;
}

void gfx_flip(void)
{
	/* the page Onion shows, turned 180 degrees */
	uint32_t *page = (uint32_t *)(mem + var.yoffset * fix.line_length);
	int stride = fix.line_length / 4;
	for (int y = 0; y < SCR_H; y++) {
		uint32_t *dst = page + (SCR_H - 1 - y) * stride + SCR_W - 1;
		const uint32_t *src = screen + y * SCR_W;
		for (int x = 0; x < SCR_W; x++)
			*dst-- = 0xFF000000 | src[x];
	}
	if (fb >= 0)
		ioctl(fb, FBIOPAN_DISPLAY, &var);
}

int input_key(int *repeat)
{
	struct input_event ev;
	while (read(in, &ev, sizeof ev) == sizeof ev) {
		if (ev.type != EV_KEY || ev.value == 0)
			continue;
		/* a held button repeats only when it moves something */
		if (ev.value == 2 && ev.code != 103 && ev.code != 108 && ev.code != 105 && ev.code != 106 &&
			ev.code != 18 && ev.code != 20)
			continue;
		if (repeat)
			*repeat = ev.value == 2;
		return ev.code;
	}
	return -1;
}
