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

static void cmd_create(void)
{
    ui_clear_screen();
    ui_print_banner();
    printf("  " SYM_INFO " " COL_BOLD "Creating database…" COL_RESET "\n");
    printf("  " COL_DIM "Scanning current directory recursively with SHA3-512.\n" COL_RESET);
    printf("\n");

    scan_t *s = scan_directory();
    if (!s) {
        fprintf(stderr, "  " SYM_FAIL " Memory allocation failed.\n");
        return;
    }

    if (s->count == 0) {
        printf("  " SYM_WARN " No files found to hash.\n");
        scan_free(s);
        return;
    }

    db_t *db = db_new();
    if (!db) {
        fprintf(stderr, "  " SYM_FAIL " Memory allocation failed.\n");
        scan_free(s);
        return;
    }

    size_t done = 0;
    scan_result *r = s->head;
    while (r) {
        db_upsert(db, r->path, r->hex);
        done++;
        ui_progress(done, s->count, r->path);
        r = r->next;
    }

    /* Timestamp update */
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(db->updated, sizeof(db->updated), "%Y-%m-%dT%H:%M:%SZ", tm);

    if (db_save(db) != 0) {
        fprintf(stderr, "\n  " SYM_FAIL " Failed to write " DB_FILENAME ".\n");
    } else {
        printf("\n");
        printf("  " SYM_OK " Database created: " COL_BOLD "%zu file%s" COL_RESET " hashed.\n",
               s->count, s->count == 1 ? "" : "s");
        printf("  " COL_DIM "  Saved to " DB_FILENAME " in the current directory.\n" COL_RESET);
    }

    if (s->errors > 0)
        printf("  " SYM_WARN " " COL_YELLOW "%zu file(s) could not be read and were skipped.\n" COL_RESET,
               s->errors);

    db_free(db);
    scan_free(s);
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
    size_t n_ok  = 0;
    size_t n_mod = 0;
    size_t n_new = 0;

    /* Collect changed files into a temporary array for pretty reporting */
    typedef struct chg { scan_result *r; int is_new; } chg;
    chg *changes = NULL;
    size_t n_changes = 0;
    size_t cap_changes = 0;

    r = s->head;
    while (r) {
        db_entry *e = db_find(db, r->path);

        if (!e) {
            /* New file not previously in database */
            n_new++;
        } else if (strcmp(e->hex, r->hex) != 0) {
            /* Content changed */
            n_mod++;
        } else {
            n_ok++;
            r = r->next;
            continue;
        }

        /* Grow changes array */
        if (n_changes >= cap_changes) {
            cap_changes = cap_changes ? cap_changes * 2 : 16;
            chg *tmp = realloc(changes, cap_changes * sizeof(chg));
            if (!tmp) { /* out of memory, skip */ r = r->next; continue; }
            changes = tmp;
        }
        changes[n_changes].r      = r;
        changes[n_changes].is_new = (db_find(db, r->path) == NULL);
        n_changes++;

        r = r->next;
    }

    /* ---- Report ---- */
    printf("  " COL_BOLD "Results:\n" COL_RESET);
    printf("  " SYM_OK "  Unchanged : " COL_GREEN COL_BOLD "%zu" COL_RESET "\n", n_ok);
    printf("  " SYM_MOD "  Modified  : " COL_YELLOW COL_BOLD "%zu" COL_RESET "\n", n_mod);
    printf("  " SYM_NEW "  New files : " COL_CYAN COL_BOLD "%zu" COL_RESET "\n", n_new);
    printf("\n");

    if (n_changes == 0) {
        printf("  " SYM_OK " " COL_GREEN COL_BOLD "All files intact. No tampering detected.\n" COL_RESET "\n");
        free(changes);
        scan_free(s);
        db_free(db);
        return;
    }

    /* ---- Interactive review of each changed file ---- */
    int any_updated = 0;

    printf("  " COL_BOLD COL_YELLOW "Changes detected! Reviewing each one…\n\n" COL_RESET);

    size_t i;
    for (i = 0; i < n_changes; i++) {
        scan_result *cr = changes[i].r;
        int is_new      = changes[i].is_new;

        printf("  " COL_DIM "──────────────────────────────────────────────────\n" COL_RESET);
        if (is_new) {
            printf("  " SYM_NEW " " COL_CYAN COL_BOLD "NEW FILE\n" COL_RESET);
        } else {
            printf("  " SYM_MOD " " COL_YELLOW COL_BOLD "MODIFIED FILE\n" COL_RESET);
        }
        printf("  " COL_BOLD "  Path   : " COL_RESET "%s\n", cr->path);
        if (!is_new) {
            db_entry *e = db_find(db, cr->path);
            printf("  " COL_BOLD "  Old    : " COL_RESET COL_DIM "%.64s…\n" COL_RESET, e->hex);
            printf("  " COL_BOLD "  New    : " COL_RESET COL_DIM "%.64s…\n" COL_RESET, cr->hex);
        } else {
            printf("  " COL_BOLD "  Hash   : " COL_RESET COL_DIM "%.64s…\n" COL_RESET, cr->hex);
        }
        printf("\n");

        int ok = ui_ask_yn(is_new
            ? "Accept this new file into the database?"
            : "Accept this change as legitimate? (update database)");

        if (ok) {
            db_upsert(db, cr->path, cr->hex);
            any_updated = 1;
            printf("  " SYM_OK " Accepted.\n\n");
        } else {
            printf("  " SYM_WARN " " COL_RED "Rejected — database NOT updated for this file.\n\n" COL_RESET);
        }
    }
    printf("  " COL_DIM "──────────────────────────────────────────────────\n\n" COL_RESET);

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
        } else if (c == '3' || c == 'q' || c == 'Q') {
            ui_clear_screen();
            printf(COL_DIM "  Goodbye.\n\n" COL_RESET);
            break;
        } else if (c != '\n' && c != '\r') {
            printf("  " SYM_WARN " Invalid option. Choose 1, 2 or 3.\n\n");
            fflush(stdout);
            /* brief pause so user can read */
            struct timespec ts = {0, 800000000L};
            nanosleep(&ts, NULL);
        }
    }
    return 0;
}
