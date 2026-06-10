#ifndef SCAN_H
#define SCAN_H

#include "db.h"

typedef struct scan_result {
    char  path[DB_MAX_PATH];
    char  hex[DB_HEX_LEN + 1];
    struct scan_result *next;
} scan_result;

typedef struct {
    scan_result *head;
    scan_result *tail;   /* keeps list append O(1) */
    size_t       count;
    size_t       errors;
} scan_t;

/* Callback invoked once per regular file as soon as its hash is ready.
 * Return 0 to continue, non-zero to abort the whole scan. */
typedef int (*scan_cb)(const char *rel_path,
                       const char hex[DB_HEX_LEN + 1],
                       void *ctx);

/* Fast pre-count of regular files (readdir + lstat only, no hashing).
 * Use it to get a total for a progress bar before the slow hashing pass. */
size_t scan_count_files(void);

/* Streaming scan: recursively hash every file (except DB_FILENAME) and invoke
 * cb for each one, holding nothing in memory. *count_out / *errors_out (may be
 * NULL) receive the number of files hashed and the number skipped on error.
 * Returns 0 on success, -1 if a callback aborted the scan. */
int scan_stream(scan_cb cb, void *ctx, size_t *count_out, size_t *errors_out);

/* Recursively scan cwd, hashing every file except DB_FILENAME.
 * Returns allocated scan_t (caller must free with scan_free). */
scan_t *scan_directory(void);

void scan_free(scan_t *s);

#endif /* SCAN_H */
