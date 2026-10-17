/* The Miyoo Mini (Plus) hardware the port manager touches, emulated for tests under qemu-arm.
 *
 * Linked into the emulator build (src/build.sh with EMU=1) together with
 * -Wl,--wrap=open,--wrap=open64,--wrap=ioctl,--wrap=read -lm -lpthread, so the port manager runs unchanged:
 *   - /dev/fb0: a 640x480x32 panel with two pages in fb0.raw; each FBIOPAN_DISPLAY is one frame,
 *   - /dev/input/event0: a pipe; button events from a script are dispatched on-demand when the UI waits for input,
 *   - screenshots of the page shown at chosen frames (PPM, turned back upright),
 *   - crashes: signal, pc, lr and code addresses on the stack (for addr2line),
 *   - clean exit when quit frame is reached or when all scripted events have been processed.
 *
 * Environment (set by miyoo-emu.py):
 *   MIYOO_EMU_DIR      folder for the panel file and screenshots (default: current folder)
 *   MIYOO_EMU_INPUT    script: lines "<frame> <key> <0|1>" (key: A B X Y L1 R1 L2 R2 UP DOWN
 *                      LEFT RIGHT START SELECT MENU POWER VOLUP VOLDOWN, 1 = down, 0 = up)
 *   MIYOO_EMU_SHOTS    frames to save: "1,2,4" and/or "every:10"
 *   MIYOO_EMU_QUIT     frame at which the emulator quits cleanly (default 0: disabled)
 *   MIYOO_EMU_LOG      every N frames a progress line on stderr (default 100)
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <malloc.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PANEL_W 640
#define PANEL_H 480
#define PAGES 2

int __real_open(const char* path, int flags, ...);
int __real_open64(const char* path, int flags, ...);
int __real_ioctl(int fd, unsigned long req, ...);
ssize_t __real_read(int fd, void* buf, size_t count);

static int s_fbFd = -1;
static uint32_t* s_fbMem;			/* the shim's own view of the panel file */
static unsigned s_yoffset;
static int s_rotate = 1;

static int s_inRead = -1, s_inWrite = -1;

static unsigned s_frame;			/* the frame shown (pans so far) */
static unsigned s_quitFrame;
static unsigned s_logEvery = 100;
static size_t s_heapPeak;
static uint64_t s_startNs;

static char s_dir[512] = ".";

/* held while a screenshot is written */
static pthread_mutex_t s_shotLock = PTHREAD_MUTEX_INITIALIZER;

struct InputEvent { unsigned frame; unsigned short code; unsigned char down; };
static struct InputEvent* s_script;
static int s_scriptCount, s_scriptPos;

static unsigned* s_shots;
static int s_shotCount;
static unsigned s_shotEvery;

static uint64_t NowNs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

//----------------------------------------------------------------------------------------
// setup

static const struct { const char* name; unsigned short code; } c_keys[] = {
	/* as the Miyoo's gpio-keys report them (see gfx.h) */
	{ "UP", KEY_UP }, { "DOWN", KEY_DOWN }, { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT },
	{ "A", KEY_SPACE }, { "B", KEY_LEFTCTRL }, { "X", KEY_LEFTSHIFT }, { "Y", KEY_LEFTALT },
	{ "L1", KEY_E }, { "R1", KEY_T }, { "L2", KEY_TAB }, { "R2", KEY_BACKSPACE },
	{ "SELECT", KEY_RIGHTCTRL }, { "START", KEY_ENTER }, { "MENU", KEY_ESC },
	{ "POWER", KEY_POWER }, { "VOLUP", KEY_VOLUMEUP }, { "VOLDOWN", KEY_VOLUMEDOWN },
};

static int KeyCode(const char* name)
{
	for (size_t i = 0; i < sizeof(c_keys) / sizeof(c_keys[0]); i++)
	{
		if (!strcasecmp(c_keys[i].name, name))
			return c_keys[i].code;
	}
	return -1;
}

static int CompareEvents(const void* a, const void* b)
{
	const struct InputEvent* x = (const struct InputEvent*)a;
	const struct InputEvent* y = (const struct InputEvent*)b;
	return x->frame < y->frame ? -1 : x->frame > y->frame;
}

static void LoadScript(const char* path)
{
	FILE* fp = fopen(path, "r");
	char line[256], key[32];
	unsigned frame;
	int down, cap = 0;

	if (!fp)
	{
		fprintf(stderr, "emu: can't read the input script %s\n", path);
		return;
	}

	while (fgets(line, sizeof(line), fp))
	{
		if (line[0] == '#' || sscanf(line, "%u %31s %d", &frame, key, &down) != 3)
			continue;

		const int code = KeyCode(key);
		if (code < 0)
		{
			fprintf(stderr, "emu: unknown key '%s' in the input script\n", key);
			continue;
		}

		if (s_scriptCount == cap)
		{
			cap = cap ? cap * 2 : 64;
			s_script = (struct InputEvent*)realloc(s_script, cap * sizeof(*s_script));
		}
		s_script[s_scriptCount].frame = frame;
		s_script[s_scriptCount].code = (unsigned short)code;
		s_script[s_scriptCount].down = (unsigned char)(down != 0);
		s_scriptCount++;
	}
	fclose(fp);

	/* stable for events of one frame: insertion sort (scripts are short) */
	for (int i = 1; i < s_scriptCount; i++)
	{
		struct InputEvent e = s_script[i];
		int j = i - 1;
		while (j >= 0 && CompareEvents(&s_script[j], &e) > 0)
		{
			s_script[j + 1] = s_script[j];
			j--;
		}
		s_script[j + 1] = e;
	}
}

static void ParseShots(const char* list)
{
	const char* p = list;
	int cap = 0;

	while (*p)
	{
		if (!strncmp(p, "every:", 6))
		{
			s_shotEvery = (unsigned)strtoul(p + 6, (char**)&p, 10);
		}
		else
		{
			char* end;
			const unsigned f = (unsigned)strtoul(p, &end, 10);
			if (end == p)
				break;
			if (s_shotCount == cap)
			{
				cap = cap ? cap * 2 : 32;
				s_shots = (unsigned*)realloc(s_shots, cap * sizeof(*s_shots));
			}
			s_shots[s_shotCount++] = f;
			p = end;
		}
		while (*p == ',' || *p == ' ')
			p++;
	}
}

static void CrashHandler(int sig, siginfo_t* info, void* ctx);
static void AtExit(void);

__attribute__((constructor)) static void EmuInit(void)
{
	const char* v;

	s_startNs = NowNs();

	if ((v = getenv("MIYOO_EMU_DIR")) && *v)
		snprintf(s_dir, sizeof(s_dir), "%s", v);
	if ((v = getenv("MIYOO_EMU_INPUT")) && *v)
		LoadScript(v);
	if ((v = getenv("MIYOO_EMU_SHOTS")) && *v)
		ParseShots(v);
	if ((v = getenv("MIYOO_EMU_QUIT")) && *v)
		s_quitFrame = (unsigned)atoi(v);
	if ((v = getenv("MIYOO_EMU_LOG")))
		s_logEvery = (unsigned)atoi(v);
	if ((v = getenv("MIYOO_ROTATE")))
		s_rotate = atoi(v);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = CrashHandler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);

	atexit(AtExit);

	fprintf(stderr, "emu: Miyoo Mini emulation: %d scripted button events, quit at frame %u\n", s_scriptCount, s_quitFrame);
}

//----------------------------------------------------------------------------------------
// the panel

static int OpenPanel(void)
{
	char path[600];
	const size_t size = (size_t)PANEL_W * 4 * PANEL_H * PAGES;

	snprintf(path, sizeof(path), "%s/fb0.raw", s_dir);
	const int fd = __real_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
	{
		fprintf(stderr, "emu: can't create the panel file %s (%s)\n", path, strerror(errno));
		return -1;
	}

	if (ftruncate(fd, (off_t)size) < 0)
	{
		close(fd);
		return -1;
	}

	s_fbMem = (uint32_t*)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (s_fbMem == MAP_FAILED)
		s_fbMem = NULL;

	s_fbFd = fd;
	s_yoffset = 0;
	fprintf(stderr, "emu: /dev/fb0 -> %s (%dx%d, 32 bpp, %d pages)\n", path, PANEL_W, PANEL_H, PAGES);
	return fd;
}

static void FillVar(struct fb_var_screeninfo* v)
{
	memset(v, 0, sizeof(*v));
	v->xres = v->xres_virtual = PANEL_W;
	v->yres = PANEL_H;
	v->yres_virtual = PANEL_H * PAGES;
	v->yoffset = s_yoffset;
	v->bits_per_pixel = 32;
	v->transp.offset = 24; v->transp.length = 8;
	v->red.offset = 16; v->red.length = 8;
	v->green.offset = 8; v->green.length = 8;
	v->blue.offset = 0; v->blue.length = 8;
	v->width = 61;	/* mm, 2.8\" 4:3 */
	v->height = 46;
}

static void FillFix(struct fb_fix_screeninfo* f)
{
	memset(f, 0, sizeof(*f));
	strcpy(f->id, "miyoo-emu");
	f->smem_len = PANEL_W * 4 * PANEL_H * PAGES;
	f->type = FB_TYPE_PACKED_PIXELS;
	f->visual = FB_VISUAL_TRUECOLOR;
	f->ypanstep = 1;
	f->line_length = PANEL_W * 4;
}

/* the page shown, as the player sees it (turned back upright if s_rotate), as a binary PPM */
static void SaveShot(unsigned frame)
{
	char path[600], part[610];
	static uint8_t row[PANEL_W * 3];

	if (!s_fbMem)
		return;

	pthread_mutex_lock(&s_shotLock);

	/* written as .part and renamed: never a cut-off picture under the final name */
	snprintf(path, sizeof(path), "%s/shot_%05u.ppm", s_dir, frame);
	snprintf(part, sizeof(part), "%s.part", path);
	FILE* fp = fopen(part, "wb");
	if (!fp)
	{
		pthread_mutex_unlock(&s_shotLock);
		return;
	}

	fprintf(fp, "P6\n%d %d\n255\n", PANEL_W, PANEL_H);
	const uint32_t* page = s_fbMem + (s_yoffset / PANEL_H) * PANEL_W * PANEL_H;
	for (int y = 0; y < PANEL_H; y++)
	{
		const uint32_t* src = page + (s_rotate ? PANEL_H - 1 - y : y) * PANEL_W;
		for (int x = 0; x < PANEL_W; x++)
		{
			const uint32_t c = src[s_rotate ? PANEL_W - 1 - x : x];
			row[x * 3 + 0] = (uint8_t)(c >> 16);
			row[x * 3 + 1] = (uint8_t)(c >> 8);
			row[x * 3 + 2] = (uint8_t)c;
		}
		fwrite(row, 1, sizeof(row), fp);
	}
	if (fclose(fp) == 0)
		rename(part, path);
	else
		unlink(part);

	pthread_mutex_unlock(&s_shotLock);
}

static void SendKey(unsigned short code, int down)
{
	struct input_event ev[2];
	struct timeval tv;

	if (s_inWrite < 0)
		return;

	gettimeofday(&tv, NULL);
	memset(ev, 0, sizeof(ev));
	ev[0].time = tv;
	ev[0].type = EV_KEY;
	ev[0].code = code;
	ev[0].value = down;
	ev[1].time = tv;
	ev[1].type = EV_SYN;
	ev[1].code = SYN_REPORT;
	if (write(s_inWrite, ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		fprintf(stderr, "emu: input pipe full, event lost\n");
}

static void TrackHeap(void)
{
	const struct mallinfo2 mi = mallinfo2();
	const size_t used = mi.arena + mi.hblkhd;
	if (used > s_heapPeak)
		s_heapPeak = used;
}

static void OnFrame(unsigned id)
{
	const unsigned last = s_frame;
	if (id == 0)
		s_frame++;
	else if (id > s_frame)
		s_frame = id;
	else
		return;

	int shot = s_shotEvery && s_frame % s_shotEvery == 0;
	for (int i = 0; i < s_shotCount && !shot; i++)
		shot = s_shots[i] == s_frame;
	if (shot)
		SaveShot(s_frame);

	if (s_quitFrame && s_frame >= s_quitFrame)
	{
		fprintf(stderr, "emu: frame %u reached (quit frame %u): exiting cleanly\n", s_frame, s_quitFrame);
		_exit(0);
	}

	if (s_frame / 30 != last / 30)
		TrackHeap();

	if (s_logEvery && s_frame / s_logEvery != last / s_logEvery)
	{
		const double secs = (NowNs() - s_startNs) / 1e9;
		fprintf(stderr, "emu: frame %u, %.1f s, heap %.1f MB\n", s_frame, secs, s_heapPeak / 1048576.0);
	}
}

static volatile int s_vblWaiters;

static void PanelWait(const char* what)
{
	static double period = -1;	/* ns, 0: answer at once */
	if (period < 0)
	{
		const char* hz = getenv("MIYOO_EMU_VSYNC_HZ");
		period = hz && atof(hz) > 0 ? 1e9 / atof(hz) : 0;
	}

	if (__sync_add_and_fetch(&s_vblWaiters, 1) > 1)
	{
		fprintf(stderr, "emu: %s while another thread waits for the panel: on the device one of them would sleep for good\n", what);
		abort();
	}

	if (period > 0)
	{
		const double now = (double)(NowNs() - s_startNs);
		const double next = (floor(now / period) + 1) * period;
		const long long ns = (long long)(next - now);
		struct timespec ts = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
		while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		{
		}
	}
	else
	{
		struct timespec ts = { 0, 200000 };
		nanosleep(&ts, NULL);
	}

	__sync_sub_and_fetch(&s_vblWaiters, 1);
}

static int PanelIoctl(unsigned long req, void* arg)
{
	switch (req)
	{
	case FBIOGET_VSCREENINFO:
		FillVar((struct fb_var_screeninfo*)arg);
		return 0;
	case FBIOGET_FSCREENINFO:
		FillFix((struct fb_fix_screeninfo*)arg);
		return 0;
	case FBIOPUT_VSCREENINFO:
	{
		struct fb_var_screeninfo* v = (struct fb_var_screeninfo*)arg;
		const unsigned yoff = v->yoffset;
		FillVar(v);
		if (yoff <= PANEL_H * (PAGES - 1))
			s_yoffset = v->yoffset = yoff;
		return 0;
	}
	case FBIOPAN_DISPLAY:
	{
		const struct fb_var_screeninfo* v = (const struct fb_var_screeninfo*)arg;
		if (v->xoffset != 0 || v->yoffset > PANEL_H * (PAGES - 1))
		{
			fprintf(stderr, "emu: FBIOPAN_DISPLAY with bad offset %u,%u\n", v->xoffset, v->yoffset);
			errno = EINVAL;
			return -1;
		}
		PanelWait("FBIOPAN_DISPLAY");
		s_yoffset = v->yoffset;
		OnFrame(v->reserved[0]);
		return 0;
	}
	case FBIO_WAITFORVSYNC:
		PanelWait("FBIO_WAITFORVSYNC");
		return 0;
	}

	fprintf(stderr, "emu: unhandled framebuffer ioctl 0x%lx\n", req);
	errno = ENOTTY;
	return -1;
}

//----------------------------------------------------------------------------------------
// the buttons

static int OpenInput(int flags)
{
	int fds[2];
	if (pipe(fds) < 0)
		return -1;

	fcntl(fds[1], F_SETFL, O_NONBLOCK);
	if (flags & O_NONBLOCK)
		fcntl(fds[0], F_SETFL, O_NONBLOCK);

	s_inRead = fds[0];
	s_inWrite = fds[1];

	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "emu: /dev/input/event0 -> scripted buttons\n");
	return s_inRead;
}

//----------------------------------------------------------------------------------------
// the wrapped calls

static unsigned char s_panelFds[1024];

static int IsPanelFd(int fd)
{
	return fd >= 0 && fd < (int)sizeof(s_panelFds) && s_panelFds[fd];
}

static int OpenCommon(const char* path, int flags, int mode, int large)
{
	if (path && !strcmp(path, "/dev/fb0"))
	{
		const int fd = s_fbFd >= 0 ? dup(s_fbFd) : OpenPanel();
		if (fd >= 0 && fd < (int)sizeof(s_panelFds))
			s_panelFds[fd] = 1;
		return fd;
	}
	if (path && !strcmp(path, "/dev/input/event0"))
		return OpenInput(flags);

	return large ? __real_open64(path, flags, mode) : __real_open(path, flags, mode);
}

int __wrap_open(const char* path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT)
	{
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	return OpenCommon(path, flags, mode, 0);
}

int __wrap_open64(const char* path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT)
	{
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	return OpenCommon(path, flags, mode, 1);
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void* arg = va_arg(ap, void*);
	va_end(ap);

	if (IsPanelFd(fd))
		return PanelIoctl(req, arg);

	return __real_ioctl(fd, req, arg);
}

ssize_t __wrap_read(int fd, void* buf, size_t count)
{
	if (fd >= 0 && fd == s_inRead)
	{
		struct pollfd pfd = { .fd = s_inRead, .events = POLLIN };
		if (poll(&pfd, 1, 0) == 0)
		{
			if (s_scriptPos >= s_scriptCount)
			{
				fprintf(stderr, "emu: all scripted events consumed (frame %u): exiting cleanly\n", s_frame);
				_exit(0);
			}
			/* Dispatch next event */
			unsigned target_frame = s_script[s_scriptPos].frame;
			while (s_scriptPos < s_scriptCount)
			{
				fprintf(stderr, "emu: frame %u (input wait): key %d %s\n", s_frame, s_script[s_scriptPos].code, s_script[s_scriptPos].down ? "down" : "up");
				SendKey(s_script[s_scriptPos].code, s_script[s_scriptPos].down);
				s_scriptPos++;
				if (s_scriptPos < s_scriptCount && s_script[s_scriptPos].frame == target_frame)
					continue;
				break;
			}
		}
	}
	return __real_read(fd, buf, count);
}

//----------------------------------------------------------------------------------------
// exit and crashes

static void AtExit(void)
{
	pthread_mutex_lock(&s_shotLock);

	TrackHeap();
	fprintf(stderr, "emu: exit after %u frames, %.1f s, heap peak %.1f MB\n",
		s_frame, (NowNs() - s_startNs) / 1e9, s_heapPeak / 1048576.0);
}

extern char __executable_start, etext;

static void CrashHandler(int sig, siginfo_t* info, void* ctx)
{
	const ucontext_t* uc = (const ucontext_t*)ctx;
	char line[200];
	int n;

	n = snprintf(line, sizeof(line), "\nemu: CRASH signal %d at frame %u, pc %08lx lr %08lx sp %08lx, address %p\n",
		sig, s_frame, uc->uc_mcontext.arm_pc, uc->uc_mcontext.arm_lr, uc->uc_mcontext.arm_sp, info->si_addr);
	write(2, line, n);

	const uintptr_t lo = (uintptr_t)&__executable_start, hi = (uintptr_t)&etext;
	const uint32_t* sp = (const uint32_t*)uc->uc_mcontext.arm_sp;
	int found = 0;
	write(2, "emu: stack", 10);
	for (int i = 0; i < 4096 && found < 32; i++)
	{
		const uint32_t v = sp[i];
		if (v >= lo && v < hi)
		{
			n = snprintf(line, sizeof(line), " %08x", v);
			write(2, line, n);
			found++;
		}
	}
	write(2, "\n", 1);

	signal(sig, SIG_DFL);
	raise(sig);
}
