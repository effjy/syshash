#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* Silence truncation: we always NUL-terminate via snprintf */
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif

/* --------------------------------------------------------------------------
 * Database file format (plain text, pipe-delimited):
 *
 *   # syshash v<version>
 *   # created: <ISO8601>
 *   # updated: <ISO8601>
 *   <hex>|<path>
 *   ...
 *
 * The hex field is a fixed 128 lowercase hex chars, so the first '|' on a
 * line is always the separator — paths containing '|' round-trip correctly.
 * Paths may NOT contain a newline (scan.c skips such files).
 * -------------------------------------------------------------------------- */

int db_path_for(const char *root, char *out, size_t out_len)
{
    int n;
    /* An empty root or "." means the current directory: use the bare filename
     * so paths stay tidy and identical to the legacy cwd behaviour. */
    if (!root || root[0] == '\0' || (root[0] == '.' && root[1] == '\0')) {
        n = snprintf(out, out_len, "%s", DB_FILENAME);
    } else {
        size_t rl = strlen(root);
        const char *sep = (rl > 0 && root[rl - 1] == '/') ? "" : "/";
        n = snprintf(out, out_len, "%s%s%s", root, sep, DB_FILENAME);
    }
    if (n < 0 || (size_t)n >= out_len) return -1;
    return 0;
}

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
    return db_load_at(".");
}

db_t *db_load_at(const char *root)
{
    char dbpath[DB_MAX_PATH];
    if (db_path_for(root, dbpath, sizeof(dbpath)) != 0) return NULL;

    FILE *fp = fopen(dbpath, "r");
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

        /* Validate hex digits; reject malformed lines */
        size_t hi;
        for (hi = 0; hi < DB_HEX_LEN; hi++)
            if (!isxdigit((unsigned char)hex[hi])) break;
        if (hi != DB_HEX_LEN) continue;

        /* A path can never legitimately contain a newline (lines are
         * newline-delimited), and an empty path is meaningless. */
        if (path[0] == '\0') continue;

        db_upsert(db, path, hex);
    }
    fclose(fp);
    return db;
}

int db_save(const db_t *db)
{
    return db_save_at(".", db);
}

int db_save_at(const char *root, const db_t *db)
{
    char dbpath[DB_MAX_PATH];
    if (db_path_for(root, dbpath, sizeof(dbpath)) != 0) return -1;

    FILE *fp = fopen(dbpath, "w");
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

    /* Detect write errors (e.g. disk full) before claiming success — a
     * silently truncated integrity database would be worse than none. */
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0)
        return -1;
    return 0;
}

/* --------------------------------------------------------------------------
 * Streaming writer
 * -------------------------------------------------------------------------- */

db_writer *db_writer_open(void)
{
    return db_writer_open_at(".");
}

db_writer *db_writer_open_at(const char *root)
{
    char dbpath[DB_MAX_PATH];
    if (db_path_for(root, dbpath, sizeof(dbpath)) != 0) return NULL;

    db_writer *w = calloc(1, sizeof(db_writer));
    if (!w) return NULL;

    w->fp = fopen(dbpath, "w");
    if (!w->fp) { free(w); return NULL; }

    char now[64];
    iso8601_now(now, sizeof(now));

    fprintf(w->fp, "# syshash v%s\n", DB_VERSION);
    fprintf(w->fp, "# created: %s\n", now);
    fprintf(w->fp, "# updated: %s\n", now);
    fprintf(w->fp, "#\n");
    fprintf(w->fp, "# Format: sha3-512-hex|relative-path\n");
    fprintf(w->fp, "# DO NOT EDIT MANUALLY\n");
    fprintf(w->fp, "#\n");

    return w;
}

int db_writer_add(db_writer *w, const char *path, const char hex[DB_HEX_LEN + 1])
{
    if (fprintf(w->fp, "%s|%s\n", hex, path) < 0)
        return -1;
    w->count++;
    return 0;
}

int db_writer_close(db_writer *w)
{
    if (!w) return -1;
    int rc = 0;
    /* Detect write errors (e.g. disk full) before claiming success — a
     * silently truncated integrity database would be worse than none. */
    if (ferror(w->fp)) rc = -1;
    if (fclose(w->fp) != 0) rc = -1;
    free(w);
    return rc;
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

    /* Append to list (O(1) via tail pointer) */
    if (!db->head) {
        db->head = e;
        db->tail = e;
    } else {
        db->tail->next = e;
        db->tail = e;
    }
    db->count++;
}

void db_remove(db_t *db, const char *path)
{
    db_entry *prev = NULL, *e = db->head;
    while (e) {
        if (strcmp(e->path, path) == 0) {
            if (prev) prev->next = e->next;
            else      db->head   = e->next;
            if (db->tail == e) db->tail = prev;
            free(e);
            db->count--;
            return;
        }
        prev = e;
        e = e->next;
    }
}
