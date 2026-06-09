#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Silence truncation: we always NUL-terminate via snprintf */
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif

/* --------------------------------------------------------------------------
 * Database file format (plain text, pipe-delimited):
 *
 *   # syshash v2.0.1
 *   # created: <ISO8601>
 *   # updated: <ISO8601>
 *   <hex>|<path>
 *   ...
 *
 * Paths may not contain '|' or newlines (POSIX portable filenames are safe).
 * -------------------------------------------------------------------------- */

static void iso8601_now(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", tm);
}

db_t *db_new(void)
{
    db_t *db = calloc(1, sizeof(db_t));
    if (!db) return NULL;
    snprintf(db->version, sizeof(db->version), "%s", DB_VERSION);
    iso8601_now(db->created, sizeof(db->created));
    snprintf(db->updated, sizeof(db->updated), "%s", db->created);
    return db;
}

void db_free(db_t *db)
{
    if (!db) return;
    db_entry *e = db->head;
    while (e) {
        db_entry *next = e->next;
        free(e);
        e = next;
    }
    free(db);
}

db_t *db_load(void)
{
    FILE *fp = fopen(DB_FILENAME, "r");
    if (!fp) return NULL;

    db_t *db = db_new();
    if (!db) { fclose(fp); return NULL; }

    char line[DB_MAX_PATH + DB_HEX_LEN + 8];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r'))
            line[--l] = '\0';

        if (line[0] == '#') {
            /* Parse header comments */
            if (strncmp(line, "# created: ", 11) == 0) {
                strncpy(db->created, line + 11, sizeof(db->created) - 1);
                db->created[sizeof(db->created) - 1] = '\0';
            } else if (strncmp(line, "# updated: ", 11) == 0) {
                strncpy(db->updated, line + 11, sizeof(db->updated) - 1);
                db->updated[sizeof(db->updated) - 1] = '\0';
            } else if (strncmp(line, "# syshash v", 11) == 0) {
                strncpy(db->version, line + 11, sizeof(db->version) - 1);
                db->version[sizeof(db->version) - 1] = '\0';
            }
            continue;
        }
        if (l == 0) continue;

        /* Format: <hex>|<path> */
        char *pipe = strchr(line, '|');
        if (!pipe) continue;
        *pipe = '\0';
        const char *hex  = line;
        const char *path = pipe + 1;

        if (strlen(hex) != DB_HEX_LEN) continue;

        db_upsert(db, path, hex);
    }
    fclose(fp);
    return db;
}

int db_save(const db_t *db)
{
    FILE *fp = fopen(DB_FILENAME, "w");
    if (!fp) return -1;

    fprintf(fp, "# syshash v%s\n", db->version);
    fprintf(fp, "# created: %s\n", db->created);
    fprintf(fp, "# updated: %s\n", db->updated);
    fprintf(fp, "#\n");
    fprintf(fp, "# Format: sha3-512-hex|relative-path\n");
    fprintf(fp, "# DO NOT EDIT MANUALLY\n");
    fprintf(fp, "#\n");

    db_entry *e = db->head;
    while (e) {
        fprintf(fp, "%s|%s\n", e->hex, e->path);
        e = e->next;
    }
    fclose(fp);
    return 0;
}

db_entry *db_find(const db_t *db, const char *path)
{
    db_entry *e = db->head;
    while (e) {
        if (strcmp(e->path, path) == 0) return e;
        e = e->next;
    }
    return NULL;
}

void db_upsert(db_t *db, const char *path, const char hex[DB_HEX_LEN + 1])
{
    db_entry *e = db_find(db, path);
    if (e) {
        strncpy(e->hex, hex, DB_HEX_LEN);
        e->hex[DB_HEX_LEN] = '\0';
        return;
    }
    e = calloc(1, sizeof(db_entry));
    if (!e) return;
    strncpy(e->path, path, DB_MAX_PATH - 1);
    strncpy(e->hex,  hex,  DB_HEX_LEN);
    e->hex[DB_HEX_LEN] = '\0';

    /* Append to list */
    if (!db->head) {
        db->head = e;
    } else {
        db_entry *tail = db->head;
        while (tail->next) tail = tail->next;
        tail->next = e;
    }
    db->count++;
}

void db_remove(db_t *db, const char *path)
{
    db_entry **pp = &db->head;
    while (*pp) {
        if (strcmp((*pp)->path, path) == 0) {
            db_entry *del = *pp;
            *pp = del->next;
            free(del);
            db->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}
