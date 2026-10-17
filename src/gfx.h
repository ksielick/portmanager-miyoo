/* Screen, font and buttons of the Miyoo Mini under OnionOS, without SDL: /dev/fb0 (the panel
 * is upside down, so everything is turned 180 degrees) and /dev/input/event0. */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#define SCR_W 640
#define SCR_H 480
#define FONT_SCALE 2
#define CW (8 * FONT_SCALE) /* character cell */

/* colours (0xRRGGBB) */
#define C_BG 0x22252B
#define C_BAR 0x2E323A
#define C_SEL 0x3D4250
#define C_TEXT 0xF0F0F0
#define C_DIM 0x9AA0AA
#define C_ACCENT 0x7B6CD9
#define C_GOOD 0x6CC08B
#define C_WARN 0xE0B050
#define C_BAD 0xE06C6C

/* inline text colour escapes */
#define CLR_TEXT   "\x01" /* white C_TEXT */
#define CLR_DIM    "\x02" /* gray C_DIM */
#define CLR_ACCENT "\x03" /* purple C_ACCENT */
#define CLR_GOOD   "\x04" /* green C_GOOD */
#define CLR_WARN   "\x05" /* amber C_WARN */
#define CLR_BAD    "\x06" /* red C_BAD */

/* the Miyoo's buttons as evdev key codes */
enum {
	K_UP = 103, K_DOWN = 108, K_LEFT = 105, K_RIGHT = 106,
	K_A = 57, K_B = 29, K_X = 42, K_Y = 56,
	K_L1 = 18, K_R1 = 20, K_L2 = 15, K_R2 = 14,
	K_SELECT = 97, K_START = 28, K_MENU = 1
};

int gfx_open(void);
void gfx_close(void);
void gfx_fill(int x, int y, int w, int h, uint32_t c);
/* draws at most max characters; returns the x after the text */
int gfx_text(int x, int y, const char *s, int max, uint32_t c);
void gfx_flip(void);

/* blocks until a button is pressed (or repeats while held); *repeat says which */
int input_key(int *repeat);

#endif
