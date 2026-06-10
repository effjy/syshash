#ifndef DB_H
#define DB_H

#include "sha3.h"
#include <stdio.h>
#include <time.h>
#include <stddef.h>

#define DB_FILENAME    ".syshash.db"
#ifndef VERSION
#define VERSION        "0.0.0"
#endif
#define DB_VERSION     VERSION
#define DB_MAX_PATH    4096
#define DB_HEX_LEN     (SHA3_512_DIGEST_SIZE * 2)

typedef struct db_entry {
    char            path[DB_MAX_PATH];
    char            hex[DB_HEX_LEN + 1];
    int             seen;   /* scratch flag used during verification */
    struct db_entry *next;
} db_entry;

typedef struct {
    char      version[32];
    char      created[64];
    char      updated[64];
    db_entry *head;
    db_entry *tail;   /* keeps db_upsert O(1) instead of O(n) */
    size_t    count;
} db_t;

db_t   *db_new(void);
void    db_free(db_t *db);

/* --------------------------------------------------------------------------
 * Streaming writer — write entries straight to disk as they are produced,
 * without buffering the whole database in memory. Used when creating a fresh
 * database over a large tree.
 * -------------------------------------------------------------------------- */
typedef struct {
    FILE  *fp;
    size_t count;
} db_writer;

/* Open DB_FILENAME for writing and emit the header. Returns NULL on failure. */
db_writer *db_writer_open(void);

/* Append one entry. Returns 0 on success, -1 on write error. */
int        db_writer_add(db_writer *w, const char *path, const char hex[DB_HEX_LEN + 1]);

/* Flush, fsync-check and close. Returns 0 on success, -1 on any write error.
 * Frees the writer regardless. */
int        db_writer_close(db_writer *w);

/* Load from DB_FILENAME in cwd; returns NULL if file absent (not an error) */
db_t   *db_load(void);

/* Write db to DB_FILENAME in cwd; returns 0 on success */
int     db_save(const db_t *db);

/* Lookup entry by path; returns pointer into list or NULL */
db_entry *db_find(const db_t *db, const char *path);

/* Insert or update an entry */
void    db_upsert(db_t *db, const char *path, const char hex[DB_HEX_LEN + 1]);

/* Remove an entry (for future use) */
void    db_remove(db_t *db, const char *path);

#endif /* DB_H */
