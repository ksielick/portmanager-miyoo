#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

void say(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (log_file) {
		fprintf(log_file, "%s\n", line);
		fflush(log_file);
	}
	if (getenv("MIYOO_EMU_DIR")) {
		fprintf(stderr, "pm: %s\n", line);
		fflush(stderr);
	}
	if (log_sink)
		log_sink(line);
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

int write_file(const char *path, const char *data, size_t len)
{
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

int remove_tree(const char *path)
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
			remove_tree(s);
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

int fetch(const char *url, const char *file)
{
	char part[1100], errlog[1100];
	snprintf(part, sizeof part, "%s.part", file);
	snprintf(errlog, sizeof errlog, "%s.err", file);
	for (int attempt = 0; attempt < 2; attempt++) {
		unlink(part);
		const char *argv[] = {wget_bin(), "-q", "-T", "15", "-O", part, url, NULL};
		if (run(argv, errlog) == 0 && file_size(part) > 0 && rename(part, file) == 0) {
			unlink(errlog);
			return 0;
		}
	}
	unlink(part);
	return -1;
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
		if (is_dir(full))
			walk(root, r, fn, ctx);
		else
			fn(r, ctx);
	}
	closedir(d);
}

void walk_files(const char *root, walk_fn fn, void *ctx)
{
	walk(root, "", fn, ctx);
}
