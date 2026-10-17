/* Files, commands, downloads, hashes and the log for the port manager. */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* every log line also goes to this callback (the screen); the log file is set by log_open */
extern void (*log_sink)(const char *line);
void log_open(const char *path);
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

char *read_file(const char *path, size_t *len); /* malloc'd, 0-terminated; NULL if missing */
int write_file(const char *path, const char *data, size_t len);
int file_exists(const char *path);
int is_dir(const char *path);
long file_size(const char *path);
int mkdirs(const char *path); /* mkdir -p */
int mkdirs_for(const char *file); /* mkdir -p of the file's folder */
int copy_file(const char *src, const char *dst); /* via dst.new, then renamed */
int copy_tree(const char *src, const char *dst); /* cp -r */
int remove_tree(const char *path); /* rm -rf */
int same_file(const char *a, const char *b); /* same contents */
int sha256_file(const char *path, char hex[65]);

/* runs argv (no shell), stdout+stderr appended to out (NULL: discarded); the exit status,
 * or -1. run_bg starts it and returns the pid (wait with run_wait). */
int run(const char *const argv[], const char *out);
int run_bg(const char *const argv[], const char *out);
int run_wait(int pid);

/* fetch <url> to <file> with Onion's wget (HTTPS); nothing left behind on failure */
int fetch(const char *url, const char *file);

/* "a/b/c" -> "c" (static buffer) */
const char *base_name(const char *path);

/* calls fn for every file under root, with its path relative to root */
typedef void (*walk_fn)(const char *rel, void *ctx);
void walk_files(const char *root, walk_fn fn, void *ctx);

#endif
