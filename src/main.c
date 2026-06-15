#include "sha3.h"
#include "db.h"
#include "scan.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

/* Drain stdin until newline (used for "press Enter" pauses). */
static void wait_enter(void)
{
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

/* --------------------------------------------------------------------------
 * Option 1 – Create / Rebuild database
 * -------------------------------------------------------------------------- */

/* Context threaded through the streaming scan so each hashed file updates the
 * progress bar and is written straight to disk. */
typedef struct {
    db_writer *w;
    size_t     total;   /* from the fast pre-count, for the bar denominator */
    size_t     done;    /* files written so far */
    int        write_failed;
} create_ctx;

static int create_on_file(const char *path, const char hex[DB_HEX_LEN + 1], void *vctx)
{
    create_ctx *c = (create_ctx *)vctx;

    if (db_writer_add(c->w, path, hex) != 0) {
        c->write_failed = 1;
        return -1;   /* abort the scan; disk write failed (e.g. full) */
    }
    c->done++;
    ui_progress(c->done, c->total, path);
    return 0;
}

static void cmd_create(void)
{
    ui_clear_screen();
    ui_print_banner();
    printf("  " SYM_INFO " " COL_BOLD "Creating database…" COL_RESET "\n");
    printf("  " COL_DIM "Scanning current directory recursively with SHA3-512.\n" COL_RESET);
    printf("\n");

    /* Pass 1 — fast count (readdir only, no hashing) so the progress bar has
     * an accurate total before the slow hashing begins. */
    printf("  " SYM_INFO " Counting files… ");
    fflush(stdout);
    size_t total = scan_count_files();
    printf(COL_BOLD "%zu" COL_RESET " found.\n\n", total);

    if (total == 0) {
        printf("  " SYM_WARN " No files found to hash.\n\n");
        return;
    }

    /* Open the database and stream entries to it as they are hashed, so memory
     * use stays flat regardless of how many files there are. */
    db_writer *w = db_writer_open();
    if (!w) {
        fprintf(stderr, "  " SYM_FAIL " Failed to open " DB_FILENAME " for writing.\n\n");
        return;
    }

    /* Pass 2 — hash every file, updating the bar in real time. */
    create_ctx ctx = { w, total, 0, 0 };
    size_t hashed = 0, errors = 0;
    scan_stream(create_on_file, &ctx, &hashed, &errors);

    if (ctx.write_failed) {
        db_writer_close(w);
        fprintf(stderr, "\n  " SYM_FAIL " Write error while saving " DB_FILENAME
                        " (disk full?). Database is incomplete.\n\n");
        return;
    }

    if (db_writer_close(w) != 0) {
        fprintf(stderr, "\n  " SYM_FAIL " Failed to finalize " DB_FILENAME ".\n\n");
        return;
    }

    printf("\n");
    printf("  " SYM_OK " Database created: " COL_BOLD "%zu file%s" COL_RESET " hashed.\n",
           ctx.done, ctx.done == 1 ? "" : "s");
    printf("  " COL_DIM "  Saved to " DB_FILENAME " in the current directory.\n" COL_RESET);

    if (errors > 0)
        printf("  " SYM_WARN " " COL_YELLOW "%zu file(s) could not be read and were skipped.\n" COL_RESET,
               errors);

    printf("\n");
}

/* --------------------------------------------------------------------------
 * Option 2 – Verify integrity
 * -------------------------------------------------------------------------- */

static void cmd_verify(void)
{
    ui_clear_screen();
    ui_print_banner();
    printf("  " SYM_INFO " " COL_BOLD "Verifying integrity…" COL_RESET "\n\n");

    db_t *db = db_load();
    if (!db) {
        printf("  " SYM_WARN " No database found (" DB_FILENAME ").\n");
        printf("  " COL_DIM "Run option 1 first to create one.\n" COL_RESET "\n");
        return;
    }

    printf("  " COL_DIM "Database created : %s\n" COL_RESET, db->created);
    printf("  " COL_DIM "Database updated : %s\n" COL_RESET, db->updated);
    printf("  " COL_DIM "Entries on record: %zu\n\n" COL_RESET, db->count);

    scan_t *s = scan_directory();
    if (!s) {
        fprintf(stderr, "  " SYM_FAIL " Memory allocation failed.\n");
        db_free(db);
        return;
    }

    /* ---- First pass: show progress while scanning ---- */
    printf("  Scanning…\n");
    size_t done = 0;
    scan_result *r = s->head;
    while (r) {
        done++;
        ui_progress(done, s->count, r->path);
        r = r->next;
    }
    printf("\n");

    /* ---- Second pass: compare ---- */
    enum { CHG_NEW, CHG_MOD, CHG_MISSING };

    size_t n_ok      = 0;
    size_t n_mod     = 0;
    size_t n_new     = 0;
    size_t n_missing = 0;

    /* Collect changed files into a temporary array for pretty reporting.
     *   r  : scan result (NULL for missing files)
     *   e  : matching db entry (NULL for new files)
     *   type: CHG_NEW / CHG_MOD / CHG_MISSING */
    typedef struct chg { scan_result *r; db_entry *e; int type; } chg;
    chg *changes = NULL;
    size_t n_changes = 0;
    size_t cap_changes = 0;

    r = s->head;
    while (r) {
        db_entry *e = db_find(db, r->path);
        if (e) e->seen = 1;   /* mark so we can find deleted files later */

        int type;
        if (!e) {
            type = CHG_NEW;             /* file not previously in database */
        } else if (strcmp(e->hex, r->hex) != 0) {
            type = CHG_MOD;             /* content changed */
        } else {
            n_ok++;
            r = r->next;
            continue;
        }

        /* Grow changes array; only count the change if we can record it,
         * so the summary totals never drift from the list contents. */
        if (n_changes >= cap_changes) {
            size_t new_cap = cap_changes ? cap_changes * 2 : 16;
            chg *tmp = realloc(changes, new_cap * sizeof(chg));
            if (!tmp) { r = r->next; continue; }  /* OOM: skip silently */
            changes = tmp;
            cap_changes = new_cap;
        }
        changes[n_changes].r    = r;
        changes[n_changes].e    = e;
        changes[n_changes].type = type;
        n_changes++;
        if (type == CHG_NEW) n_new++; else n_mod++;

        r = r->next;
    }

    /* ---- Detect missing files: db entries never seen on disk ---- */
    for (db_entry *e = db->head; e; e = e->next) {
        if (e->seen) continue;

        if (n_changes >= cap_changes) {
            size_t new_cap = cap_changes ? cap_changes * 2 : 16;
            chg *tmp = realloc(changes, new_cap * sizeof(chg));
            if (!tmp) continue;  /* OOM: skip silently */
            changes = tmp;
            cap_changes = new_cap;
        }
        changes[n_changes].r    = NULL;
        changes[n_changes].e    = e;
        changes[n_changes].type = CHG_MISSING;
        n_changes++;
        n_missing++;
    }

    /* ---- Report ---- */
    printf("  " COL_BOLD "Results:\n" COL_RESET);
    printf("  " SYM_OK "  Unchanged : " COL_GREEN COL_BOLD "%zu" COL_RESET "\n", n_ok);
    printf("  " SYM_MOD "  Modified  : " COL_YELLOW COL_BOLD "%zu" COL_RESET "\n", n_mod);
    printf("  " SYM_NEW "  New files : " COL_CYAN COL_BOLD "%zu" COL_RESET "\n", n_new);
    printf("  " SYM_DEL "  Missing   : " COL_RED COL_BOLD "%zu" COL_RESET "\n", n_missing);
    printf("\n");

    if (s->errors > 0) {
        printf("  " SYM_WARN " " COL_YELLOW "%zu file(s) could not be read and were skipped;\n"
               "          they may appear above as \"Missing\". Do not accept those\n"
               "          removals unless the files are genuinely gone.\n" COL_RESET "\n",
               s->errors);
    }

    if (n_changes == 0) {
        printf("  " SYM_OK " " COL_GREEN COL_BOLD "All files intact. No tampering detected.\n" COL_RESET "\n");
        free(changes);
        scan_free(s);
        db_free(db);
        return;
    }

    /* ---- Batch action menu ---- */
    int any_updated = 0;

    printf("  " COL_BOLD COL_YELLOW "%zu change%s detected.\n\n" COL_RESET,
           n_changes, n_changes == 1 ? "" : "s");

    size_t i;
    char action;
    for (;;) {
        printf("  " COL_BOLD "What would you like to do?\n\n" COL_RESET);
        printf("  " COL_CYAN "l" COL_RESET "  →  List all changes\n");
        printf("  " COL_CYAN "r" COL_RESET "  →  Review one by one\n");
        printf("  " COL_CYAN "a" COL_RESET "  →  Accept all changes\n");
        printf("  " COL_CYAN "s" COL_RESET "  →  Skip all (leave database unchanged)\n");
        printf("\n");
        action = ui_ask_choice("Action", "lras");
        printf("\n");

        if (action != 'l') break;

        /* ---- List view ---- */
        printf("  " COL_DIM "──────────────────────────────────────────────────\n" COL_RESET);
        for (i = 0; i < n_changes; i++) {
            switch (changes[i].type) {
            case CHG_NEW:
                printf("  " SYM_NEW "  %s\n", changes[i].r->path);
                break;
            case CHG_MOD:
                printf("  " SYM_MOD "  %s\n", changes[i].r->path);
                break;
            case CHG_MISSING:
                printf("  " SYM_DEL "  %s\n", changes[i].e->path);
                break;
            }
        }
        printf("  " COL_DIM "──────────────────────────────────────────────────\n\n" COL_RESET);
    }

    if (action == 's') {
        printf("  " SYM_INFO " Skipped — database left untouched.\n\n");
        free(changes);
        scan_free(s);
        db_free(db);
        return;
    }

    if (action == 'a') {
        /* Accept all without per-file prompts */
        for (i = 0; i < n_changes; i++) {
            if (changes[i].type == CHG_MISSING)
                db_remove(db, changes[i].e->path);
            else
                db_upsert(db, changes[i].r->path, changes[i].r->hex);
        }
        any_updated = 1;
        printf("  " SYM_OK " All %zu change%s accepted.\n\n",
               n_changes, n_changes == 1 ? "" : "s");
    } else {
        /* Per-file review */
        printf("  " COL_BOLD COL_YELLOW "Reviewing each change…\n\n" COL_RESET);

        for (i = 0; i < n_changes; i++) {
            scan_result *cr = changes[i].r;
            db_entry    *ce = changes[i].e;
            int          type = changes[i].type;
            const char  *path = (type == CHG_MISSING) ? ce->path : cr->path;

            printf("  " COL_DIM "──────────────────────────────────────────────────\n" COL_RESET);
            if (type == CHG_NEW) {
                printf("  " SYM_NEW " " COL_CYAN COL_BOLD "NEW FILE\n" COL_RESET);
            } else if (type == CHG_MOD) {
                printf("  " SYM_MOD " " COL_YELLOW COL_BOLD "MODIFIED FILE\n" COL_RESET);
            } else {
                printf("  " SYM_DEL " " COL_RED COL_BOLD "MISSING FILE\n" COL_RESET);
            }
            printf("  " COL_BOLD "  Path   : " COL_RESET "%s\n", path);
            if (type == CHG_MOD) {
                printf("  " COL_BOLD "  Old    : " COL_RESET COL_DIM "%.64s…\n" COL_RESET, ce->hex);
                printf("  " COL_BOLD "  New    : " COL_RESET COL_DIM "%.64s…\n" COL_RESET, cr->hex);
            } else if (type == CHG_NEW) {
                printf("  " COL_BOLD "  Hash   : " COL_RESET COL_DIM "%.64s…\n" COL_RESET, cr->hex);
            } else {
                printf("  " COL_BOLD "  Old    : " COL_RESET COL_DIM "%.64s…\n" COL_RESET, ce->hex);
                printf("  " COL_DIM "  (file no longer present on disk)\n" COL_RESET);
            }
            printf("\n");

            const char *q;
            if (type == CHG_NEW)      q = "Accept this new file into the database?";
            else if (type == CHG_MOD) q = "Accept this change as legitimate? (update database)";
            else                      q = "Remove this missing file from the database?";

            int ok = ui_ask_yn(q);

            if (ok) {
                if (type == CHG_MISSING)
                    db_remove(db, ce->path);
                else
                    db_upsert(db, cr->path, cr->hex);
                any_updated = 1;
                printf("  " SYM_OK " Accepted.\n\n");
            } else {
                printf("  " SYM_WARN " " COL_RED "Rejected — database NOT updated for this file.\n\n" COL_RESET);
            }
        }
        printf("  " COL_DIM "──────────────────────────────────────────────────\n\n" COL_RESET);
    }

    if (any_updated) {
        /* Update timestamp */
        time_t t = time(NULL);
        struct tm *tm = gmtime(&t);
        strftime(db->updated, sizeof(db->updated), "%Y-%m-%dT%H:%M:%SZ", tm);

        if (db_save(db) != 0)
            fprintf(stderr, "  " SYM_FAIL " Failed to write updated database.\n");
        else
            printf("  " SYM_OK " Database updated successfully.\n\n");
    } else {
        printf("  " SYM_INFO " No changes accepted — database left untouched.\n\n");
    }

    free(changes);
    scan_free(s);
    db_free(db);
}

/* --------------------------------------------------------------------------
 * Main loop
 * -------------------------------------------------------------------------- */

int main(void)
{
    char buf[8];

    for (;;) {
        ui_clear_screen();
        ui_print_banner();
        ui_print_menu();

        printf("  " COL_BOLD "Choice" COL_RESET ": ");
        fflush(stdout);

        if (!fgets(buf, sizeof(buf), stdin)) break;
        /* Drain any leftover characters if the line exceeded the buffer. */
        if (!strchr(buf, '\n')) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF)
                ;
        }
        char c = buf[0];

        if (c == '1') {
            cmd_create();
            printf("  Press " COL_DIM "[Enter]" COL_RESET " to return to menu… ");
            fflush(stdout);
            wait_enter();
        } else if (c == '2') {
            cmd_verify();
            printf("  Press " COL_DIM "[Enter]" COL_RESET " to return to menu… ");
            fflush(stdout);
            wait_enter();
        } else if (c == '3') {
            ui_print_about();
            printf("  Press " COL_DIM "[Enter]" COL_RESET " to return to menu… ");
            fflush(stdout);
            wait_enter();
        } else if (c == '4' || c == 'q' || c == 'Q') {
            ui_clear_screen();
            printf(COL_DIM "  Goodbye.\n\n" COL_RESET);
            break;
        } else if (c != '\n' && c != '\r') {
            printf("  " SYM_WARN " Invalid option. Choose 1, 2, 3 or 4.\n\n");
            fflush(stdout);
            /* brief pause so user can read */
            struct timespec ts = {0, 800000000L};
            nanosleep(&ts, NULL);
        }
    }
    return 0;
}
