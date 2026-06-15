#include "scan.h"
#include "sha3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* --------------------------------------------------------------------------
 * Recursive directory walker (callback-based)
 *
 * The walk is the expensive part: every regular file is read and hashed with
 * SHA3-512. Rather than buffer all results in memory, the walker invokes a
 * caller-supplied callback for each file as soon as its hash is ready. This
 * lets callers stream straight to disk (create) or accumulate a list (verify)
 * without the walker caring which.
 * -------------------------------------------------------------------------- */

static int walk(const char *base_prefix, const char *dir_path,
                scan_cb cb, void *ctx, size_t *count, size_t *errors)
{
    DIR *dp = opendir(dir_path);
    if (!dp) {
        fprintf(stderr, "  warning: cannot open dir: %s (%s)\n",
                dir_path, strerror(errno));
        (*errors)++;
        return 0;
    }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        /* Build full path for stat/open */
        char full[DB_MAX_PATH];
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name) >= (int)sizeof(full)) {
            (*errors)++;
            continue;
        }

        /* Build relative path (strip leading "./") */
        char rel[DB_MAX_PATH];
        const char *prefix = base_prefix[0] ? base_prefix : "";
        if (prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", prefix, de->d_name);
        else
            snprintf(rel, sizeof(rel), "%s", de->d_name);

        struct stat st;
        if (lstat(full, &st) != 0) {
            fprintf(stderr, "  warning: cannot stat: %s\n", full);
            (*errors)++;
            continue;
        }

        /* Note: lstat() is used deliberately, so symbolic links are neither
         * followed (avoids loops / escaping the tree) nor hashed. Only real
         * directories and regular files are considered. */
        if (S_ISDIR(st.st_mode)) {
            if (walk(rel, full, cb, ctx, count, errors) != 0) {
                closedir(dp);
                return -1;   /* propagate abort */
            }
        } else if (S_ISREG(st.st_mode)) {
            /* Skip the database file itself, in any directory */
            if (strcmp(de->d_name, DB_FILENAME) == 0)
                continue;

            /* A newline in a path would corrupt the line-based DB format. */
            if (strchr(rel, '\n')) {
                fprintf(stderr, "  warning: skipping path with newline: %s\n", full);
                (*errors)++;
                continue;
            }

            uint8_t digest[SHA3_512_DIGEST_SIZE];
            if (sha3_512_file(full, digest) != 0) {
                fprintf(stderr, "  warning: cannot hash: %s\n", full);
                (*errors)++;
                continue;
            }

            char hex[DB_HEX_LEN + 1];
            sha3_512_hex(digest, hex);

            (*count)++;
            if (cb(rel, hex, ctx) != 0) {
                closedir(dp);
                return -1;   /* caller asked to abort */
            }
        }
    }
    closedir(dp);
    return 0;
}

/* --------------------------------------------------------------------------
 * Fast pre-count: walk the tree counting regular files WITHOUT hashing them.
 *
 * readdir + lstat only, so this is orders of magnitude faster than hashing.
 * It gives an accurate denominator for the progress bar before the real
 * (slow) hashing pass begins.
 * -------------------------------------------------------------------------- */

static void count_walk(const char *dir_path, size_t *count)
{
    DIR *dp = opendir(dir_path);
    if (!dp) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char full[DB_MAX_PATH];
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name) >= (int)sizeof(full))
            continue;

        struct stat st;
        if (lstat(full, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            count_walk(full, count);
        } else if (S_ISREG(st.st_mode)) {
            if (strcmp(de->d_name, DB_FILENAME) == 0)
                continue;
            (*count)++;
        }
    }
    closedir(dp);
}

size_t scan_count_files(void)
{
    return scan_count_files_at(".");
}

size_t scan_count_files_at(const char *root)
{
    size_t count = 0;
    count_walk((root && root[0]) ? root : ".", &count);
    return count;
}

int scan_stream(scan_cb cb, void *ctx, size_t *count_out, size_t *errors_out)
{
    return scan_stream_at(".", cb, ctx, count_out, errors_out);
}

int scan_stream_at(const char *root, scan_cb cb, void *ctx,
                   size_t *count_out, size_t *errors_out)
{
    size_t count = 0, errors = 0;
    int rc = walk("", (root && root[0]) ? root : ".", cb, ctx, &count, &errors);
    if (count_out)  *count_out  = count;
    if (errors_out) *errors_out = errors;
    return rc;
}

/* --------------------------------------------------------------------------
 * List-building convenience wrapper (used by verification, which must hold
 * every result in memory to diff against the stored database).
 * -------------------------------------------------------------------------- */

static int list_append_cb(const char *rel, const char hex[DB_HEX_LEN + 1], void *ctx)
{
    scan_t *s = (scan_t *)ctx;

    scan_result *r = calloc(1, sizeof(scan_result));
    if (!r) { s->errors++; return 0; }   /* skip this file, keep going */

    snprintf(r->path, DB_MAX_PATH, "%s", rel);
    snprintf(r->hex, sizeof(r->hex), "%s", hex);

    if (!s->head) {
        s->head = r;
        s->tail = r;
    } else {
        s->tail->next = r;
        s->tail = r;
    }
    s->count++;
    return 0;
}

scan_t *scan_directory(void)
{
    return scan_directory_at(".");
}

scan_t *scan_directory_at(const char *root)
{
    scan_t *s = calloc(1, sizeof(scan_t));
    if (!s) return NULL;
    size_t count = 0, errors = 0;
    walk("", (root && root[0]) ? root : ".", list_append_cb, s, &count, &errors);
    s->errors += errors;   /* walk-level errors (unreadable dirs/files) */
    return s;
}

void scan_free(scan_t *s)
{
    if (!s) return;
    scan_result *r = s->head;
    while (r) {
        scan_result *next = r->next;
        free(r);
        r = next;
    }
    free(s);
}
