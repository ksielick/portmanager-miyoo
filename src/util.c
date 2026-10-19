#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vendor/sha256.h"

void (*log_sink)(const char *line);
static FILE *log_file;

void log_open(const char *path)
{
	log_file = fopen(path, "a");
}

static void vsay(int to_log, int to_screen, const char *fmt, va_list ap)
{
	char line[1024];
	vsnprintf(line, sizeof line, fmt, ap);
	if (to_log && log_file) {
		fprintf(log_file, "%s\n", line);
		fflush(log_file);
	}
	if (to_log && getenv("MIYOO_EMU_DIR")) {
		fprintf(stderr, "pm: %s\n", line);
		fflush(stderr);
	}
	if (to_screen && log_sink)
		log_sink(line);
}

void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsay(1, 1, fmt, ap);
	va_end(ap);
}

void say_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsay(1, 0, fmt, ap);
	va_end(ap);
}

void say_screen(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsay(0, 1, fmt, ap);
	va_end(ap);
}

char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *d = malloc(n + 1);
	if (d && fread(d, 1, n, f) != (size_t)n) {
		free(d);
		d = NULL;
	}
	fclose(f);
	if (!d)
		return NULL;
	d[n] = 0;
	if (len)
		*len = n;
	return d;
}

static char guard_roots[8][600];
static int nguard;

/* the absolute path with links and ".." resolved, also for a file not there yet */
static int canon(const char *path, char *out, size_t size)
{
	char p[1024], tail[1024] = "";
	snprintf(p, sizeof p, "%s", path);
	for (;;) {
		char r[PATH_MAX];
		if (realpath(p, r)) {
			snprintf(out, size, "%s%s", r, tail);
			return 0;
		}
		char *s = strrchr(p, '/');
		if (!s || s == p)
			return -1;
		/* what isn't there yet may not climb back out */
		if (!strcmp(s, "/..") || !strcmp(s, "/."))
			return -1;
		char t[1024];
		snprintf(t, sizeof t, "%s%s", s, tail);
		snprintf(tail, sizeof tail, "%s", t);
		*s = 0;
	}
}

void guard_allow(const char *dir)
{
	mkdirs(dir);
	if (nguard < 8 && canon(dir, guard_roots[nguard], sizeof guard_roots[nguard]) == 0)
		nguard++;
}

int guard_ok(const char *path)
{
	if (!nguard)
		return 1;
	char c[1100];
	if (canon(path, c, sizeof c) == 0)
		for (int i = 0; i < nguard; i++) {
			size_t n = strlen(guard_roots[i]);
			if (!strncmp(c, guard_roots[i], n) && c[n] == '/' && c[n + 1])
				return 1;
		}
	say("BLOCKED %s: outside Port Manager's folders, left as it is", path);
	errno = EPERM;
	return 0;
}

int del_file(const char *path)
{
	return guard_ok(path) ? unlink(path) : -1;
}

long long free_bytes(const char *path)
{
	/* the emulator's scenarios can make the card nearly full */
	const char *emu = getenv("MIYOO_EMU_DIR") ? getenv("MIYOO_EMU_FREE_MB") : NULL;
	if (emu)
		return atoll(emu) * 1024 * 1024;
	struct statvfs v;
	if (statvfs(path, &v) != 0)
		return -1;
	return (long long)v.f_bavail * v.f_frsize;
}

static void add_size(const char *rel, void *ctx)
{
	long long *sum = ((long long **)ctx)[0];
	const char *root = ((const char **)ctx)[1];
	char full[2048];
	snprintf(full, sizeof full, "%s/%s", root, rel);
	long s = file_size(full);
	if (s > 0)
		*sum += s;
}

long long tree_bytes(const char *path)
{
	if (!is_dir(path)) {
		long s = file_size(path);
		return s > 0 ? s : 0;
	}
	long long sum = 0;
	void *ctx[2] = {&sum, (void *)path};
	walk_files(path, add_size, ctx);
	return sum;
}

int write_file(const char *path, const char *data, size_t len)
{
	if (!guard_ok(path))
		return -1;
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	int ok = fwrite(data, 1, len, f) == len;
	return fclose(f) == 0 && ok ? 0 : -1;
}

int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

long file_size(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

int mkdirs(const char *path)
{
	char p[1024];
	snprintf(p, sizeof p, "%s", path);
	for (char *s = p + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0777);
			*s = '/';
		}
	return mkdir(p, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

int mkdirs_for(const char *file)
{
	char p[1024];
	snprintf(p, sizeof p, "%s", file);
	char *s = strrchr(p, '/');
	if (!s)
		return 0;
	*s = 0;
	return mkdirs(p);
}

int copy_file(const char *src, const char *dst)
{
	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.new", dst);
	if (!guard_ok(dst))
		return -1;
	FILE *in = fopen(src, "rb");
	if (!in)
		return -1;
	mkdirs_for(dst);
	FILE *out = fopen(tmp, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	char buf[65536];
	size_t n;
	int ok = 1;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		if (fwrite(buf, 1, n, out) != n)
			ok = 0;
	fclose(in);
	if (fclose(out) != 0)
		ok = 0;
	struct stat st;
	if (ok && stat(src, &st) == 0)
		chmod(tmp, st.st_mode & 0777);
	if (!ok || rename(tmp, dst) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int move_file(const char *src, const char *dst)
{
	if (!guard_ok(src) || !guard_ok(dst))
		return -1;
	mkdirs_for(dst);
	if (rename(src, dst) == 0)
		return 0;
	/* another filesystem, or the card refused: copy it instead */
	if (copy_file(src, dst))
		return -1;
	unlink(src);
	return 0;
}

int copy_tree(const char *src, const char *dst)
{
	if (!is_dir(src))
		return copy_file(src, dst);
	DIR *d = opendir(src);
	if (!d)
		return -1;
	mkdirs(dst);
	int r = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char s[1024], t[1024];
		snprintf(s, sizeof s, "%s/%s", src, e->d_name);
		snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
		if (copy_tree(s, t))
			r = -1;
	}
	closedir(d);
	return r;
}

static int remove_inside(const char *path);

int remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return 0;
	if (!guard_ok(path))
		return -1;
	return remove_inside(path);
}

/* below a path the guard let through: links are removed, never followed */
static int remove_inside(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return 0;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char s[1024];
			snprintf(s, sizeof s, "%s/%s", path, e->d_name);
			remove_inside(s);
		}
		if (d)
			closedir(d);
		return rmdir(path);
	}
	return unlink(path);
}

int same_file(const char *a, const char *b)
{
	FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
	int same = fa && fb;
	static char ba[65536], bb[65536];
	while (same) {
		size_t na = fread(ba, 1, sizeof ba, fa), nb = fread(bb, 1, sizeof bb, fb);
		if (na != nb || memcmp(ba, bb, na))
			same = 0;
		if (na == 0)
			break;
	}
	if (fa)
		fclose(fa);
	if (fb)
		fclose(fb);
	return same;
}

int sha256_file(const char *path, char hex[65])
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	SHA256_CTX ctx;
	sha256_init(&ctx);
	static BYTE buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		sha256_update(&ctx, buf, n);
	fclose(f);
	BYTE h[SHA256_BLOCK_SIZE];
	sha256_final(&ctx, h);
	for (int i = 0; i < SHA256_BLOCK_SIZE; i++)
		sprintf(hex + 2 * i, "%02x", h[i]);
	return 0;
}

int run_bg(const char *const argv[], const char *out)
{
	pid_t pid = fork();
	if (pid != 0)
		return pid;
	int fd = open(out ? out : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0666);
	int nul = open("/dev/null", O_RDONLY);
	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
	}
	if (nul >= 0)
		dup2(nul, 0);
	execvp(argv[0], (char *const *)argv);
	_exit(127);
}

int run_wait(int pid)
{
	int st;
	if (pid < 0 || waitpid(pid, &st, 0) < 0)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int run(const char *const argv[], const char *out)
{
	return run_wait(run_bg(argv, out));
}

const char *wget_bin(void)
{
	static char path[512];
	const char *sd = getenv("PM_SD");
	if (sd && *sd) {
		snprintf(path, sizeof path, "%s/.tmp_update/bin/wget", sd);
		if (access(path, X_OK) == 0)
			return path;
	}
	if (access("/mnt/SDCARD/.tmp_update/bin/wget", X_OK) == 0)
		return "/mnt/SDCARD/.tmp_update/bin/wget";
	return "wget";
}

static char g_ca_cert[600];

void set_ca_cert(const char *path)
{
	if (path && *path)
		snprintf(g_ca_cert, sizeof g_ca_cert, "%s", path);
	else
		g_ca_cert[0] = 0;
}

const char *wget_ca_cert(void)
{
	if (*g_ca_cert && file_exists(g_ca_cert))
		return g_ca_cert;
	return NULL;
}

progress_cb on_download_progress = NULL;
progress_cb on_extract_progress = NULL;
progress_cb on_copy_progress = NULL;
progress_cb on_check_progress = NULL;
file_cb on_file_installed = NULL;
file_cb on_stage = NULL;

const char *curl_bin(void)
{
	static char path[512];
	const char *sd = getenv("PM_SD");
	if (sd && *sd) {
		snprintf(path, sizeof path, "%s/.tmp_update/bin/curl", sd);
		if (access(path, X_OK) == 0)
			return path;
	}
	if (access("/mnt/SDCARD/.tmp_update/bin/curl", X_OK) == 0)
		return "/mnt/SDCARD/.tmp_update/bin/curl";
	return "curl";
}

int fetch_auth(const char *url, const char *file, long total_bytes, const char *auth_token, const char *extra_header)
{
	char part[1100], errlog[1100];
	snprintf(part, sizeof part, "%s.part", file);
	snprintf(errlog, sizeof errlog, "%s.err", file);
	const char *to = total_bytes > 0 ? "60" : "15";
	const char *ca = wget_ca_cert();

	/* If auth or custom headers are needed, use curl */
	int use_curl = (auth_token && *auth_token) || (extra_header && *extra_header);

	char auth_hdr[300], ca_arg[700];
	if (auth_token && *auth_token)
		snprintf(auth_hdr, sizeof auth_hdr, "Authorization: Bearer %s", auth_token);
	if (ca)
		snprintf(ca_arg, sizeof ca_arg, use_curl ? "%s" : "--ca-certificate=%s", ca);

	for (int attempt = 0; attempt < 2; attempt++) {
		unlink(part);
		const char *argv[16];
		int argc = 0;
		if (use_curl) {
			argv[argc++] = curl_bin();
			argv[argc++] = "-s";
			argv[argc++] = "-S";
			argv[argc++] = "-L";
			argv[argc++] = "-m";
			argv[argc++] = to;
			if (ca) {
				argv[argc++] = "--cacert";
				argv[argc++] = ca_arg;
			}
			if (auth_token && *auth_token) {
				argv[argc++] = "-H";
				argv[argc++] = auth_hdr;
			}
			if (extra_header && *extra_header) {
				argv[argc++] = "-H";
				argv[argc++] = extra_header;
			}
			argv[argc++] = "-o";
			argv[argc++] = part;
			argv[argc++] = url;
			argv[argc] = NULL;
		} else {
			argv[argc++] = wget_bin();
			argv[argc++] = "-q";
			argv[argc++] = "-T";
			argv[argc++] = to;
			if (ca)
				argv[argc++] = ca_arg;
			argv[argc++] = "-O";
			argv[argc++] = part;
			argv[argc++] = url;
			argv[argc] = NULL;
		}

		int pid = run_bg(argv, errlog);
		if (pid < 0)
			continue;
		if (on_download_progress && total_bytes > 0)
			on_download_progress(0, total_bytes);
		int status = -1;
		long last_bytes = 0;
		while (1) {
			int st;
			pid_t res = waitpid(pid, &st, WNOHANG);
			if (res == pid) {
				status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
				break;
			}
			if (res < 0) {
				status = -1;
				break;
			}
			long cur = file_size(part);
			if (cur >= 0 && cur != last_bytes) {
				last_bytes = cur;
				if (on_download_progress && (total_bytes > 0 || cur > 0))
					on_download_progress(cur, total_bytes);
			}
			usleep(60000); /* 60 ms */
		}
		if (status == 0 && file_size(part) > 0 && rename(part, file) == 0) {
			if (on_download_progress && total_bytes > 0)
				on_download_progress(total_bytes, total_bytes);
			unlink(errlog);
			return 0;
		}
	}
	if (on_download_progress)
		on_download_progress(0, 0);
	unlink(part);
	return -1;
}

int fetch(const char *url, const char *file, long total_bytes)
{
	return fetch_auth(url, file, total_bytes, NULL, NULL);
}

const char *base_name(const char *path)
{
	static char b[256];
	const char *s = strrchr(path, '/');
	snprintf(b, sizeof b, "%s", s ? s + 1 : path);
	return b;
}

static void walk(const char *root, const char *rel, walk_fn fn, void *ctx)
{
	char dir[1024];
	snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char r[1024], full[2048];
		snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
		snprintf(full, sizeof full, "%s/%s", root, r);
		struct stat st;
		if (lstat(full, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			walk(root, r, fn, ctx);
		else if (S_ISREG(st.st_mode))
			fn(r, ctx);
	}
	closedir(d);
}

void walk_files(const char *root, walk_fn fn, void *ctx)
{
	walk(root, "", fn, ctx);
}
