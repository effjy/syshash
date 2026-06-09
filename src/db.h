#ifndef DB_H
#define DB_H

#include "sha3.h"
#include <time.h>
#include <stddef.h>

#define DB_FILENAME    ".syshash.db"
#define DB_VERSION     "2.0.1"
#define DB_MAX_PATH    4096
#define DB_HEX_LEN     (SHA3_512_DIGEST_SIZE * 2)

typedef struct db_entry {
    char            path[DB_MAX_PATH];
    char            hex[DB_HEX_LEN + 1];
    struct db_entry *next;
} db_entry;

typedef struct {
    char      version[32];
    char      created[64];
    char      updated[64];
    db_entry *head;
    size_t    count;
} db_t;

db_t   *db_new(void);
void    db_free(db_t *db);

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
