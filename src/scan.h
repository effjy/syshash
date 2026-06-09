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
    size_t       count;
    size_t       errors;
} scan_t;

/* Recursively scan cwd, hashing every file except DB_FILENAME.
 * Returns allocated scan_t (caller must free with scan_free). */
scan_t *scan_directory(void);

void scan_free(scan_t *s);

#endif /* SCAN_H */
