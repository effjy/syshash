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
 * Recursive directory walker
 * -------------------------------------------------------------------------- */

static void walk(const char *base_prefix, const char *dir_path, scan_t *s)
{
    DIR *dp = opendir(dir_path);
    if (!dp) {
        fprintf(stderr, "  warning: cannot open dir: %s (%s)\n",
                dir_path, strerror(errno));
        s->errors++;
        return;
    }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        /* Build full path for stat/open */
        char full[DB_MAX_PATH];
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name) >= (int)sizeof(full)) {
            s->errors++;
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
            s->errors++;
            continue;
        }

        /* Note: lstat() is used deliberately, so symbolic links are neither
         * followed (avoids loops / escaping the tree) nor hashed. Only real
         * directories and regular files are considered. */
        if (S_ISDIR(st.st_mode)) {
            walk(rel, full, s);
        } else if (S_ISREG(st.st_mode)) {
            /* Skip the database file itself, in any directory */
            if (strcmp(de->d_name, DB_FILENAME) == 0)
                continue;

            /* A newline in a path would corrupt the line-based DB format. */
            if (strchr(rel, '\n')) {
                fprintf(stderr, "  warning: skipping path with newline: %s\n", full);
                s->errors++;
                continue;
            }

            uint8_t digest[SHA3_512_DIGEST_SIZE];
            if (sha3_512_file(full, digest) != 0) {
                fprintf(stderr, "  warning: cannot hash: %s\n", full);
                s->errors++;
                continue;
            }

            scan_result *r = calloc(1, sizeof(scan_result));
            if (!r) { s->errors++; continue; }

            snprintf(r->path, DB_MAX_PATH, "%s", rel);
            sha3_512_hex(digest, r->hex);

            /* Append to list (O(1) via tail pointer) */
            if (!s->head) {
                s->head = r;
                s->tail = r;
            } else {
                s->tail->next = r;
                s->tail = r;
            }
            s->count++;
        }
    }
    closedir(dp);
}

scan_t *scan_directory(void)
{
    scan_t *s = calloc(1, sizeof(scan_t));
    if (!s) return NULL;
    walk("", ".", s);
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
