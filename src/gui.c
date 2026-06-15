/* ==========================================================================
 * Syshash GTK3 GUI
 *
 * A graphical front-end over the same SHA3-512 file-integrity core used by the
 * command-line tool. Unlike the CLI (which always works in the current launch
 * directory), the GUI lets you pick which directory to protect:
 *
 *   • Default        — the user's home directory (and its .syshash.db).
 *   • Create / Rebuild — choose ANY directory; its database is (re)built.
 *   • Open Database  — pick an existing .syshash.db anywhere; its containing
 *                      directory becomes the monitored root.
 *
 * The model mirrors the CLI exactly: a database lives inside the directory it
 * protects, named ".syshash.db". This keeps both front-ends interchangeable.
 *
 * The slow hashing work runs in a worker thread so the UI never freezes; a
 * GTK timeout polls shared atomics to drive the progress bar.
 * ========================================================================== */

#include <gtk/gtk.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "sha3.h"
#include "db.h"
#include "scan.h"
#include "ui.h"   /* for AUTHOR + VERSION */

/* -------------------------------------------------------------------------- */

typedef enum { CHG_NEW, CHG_MOD, CHG_MISSING } chg_type;

typedef struct {
    chg_type type;
    char    *path;
    char    *old_hex;   /* may be NULL (new file)     */
    char    *new_hex;   /* may be NULL (missing file) */
} Change;

typedef enum { JOB_CREATE, JOB_VERIFY } job_kind;

typedef struct {
    job_kind  kind;
    char     *root;          /* directory being processed (owned)            */

    /* progress (written by worker, read by UI timeout) */
    volatile gint total;
    volatile gint done;
    volatile gint finished;  /* set to 1 when the worker is done             */

    /* results */
    gboolean   ok;
    char      *message;      /* human-readable outcome (owned)               */
    char      *current;      /* path of file currently hashing (owned/locked)*/
    GMutex     cur_lock;

    /* verify outcome */
    GPtrArray *changes;      /* of Change* (owned)                           */
    size_t     n_ok, n_mod, n_new, n_missing, errors;
} Job;

typedef struct {
    GtkApplication *app;
    GtkWidget      *window;
    GtkWidget      *root_label;
    GtkWidget      *status_label;
    GtkWidget      *progress;
    GtkWidget      *tree;
    GtkListStore   *store;
    GtkWidget      *apply_btn;
    GtkWidget      *btn_create;
    GtkWidget      *btn_open;
    GtkWidget      *btn_verify;

    char           *root;        /* current monitored directory (owned)      */
    GPtrArray      *changes;     /* current verify changes (owned)           */

    GThread        *worker;
    Job            *job;
    guint           poll_id;
} App;

/* columns in the changes list store */
enum { COL_ACCEPT, COL_TYPE, COL_PATH, N_COLS };

/* -------------------------------------------------------------------------- */

static void change_free(gpointer p)
{
    Change *c = p;
    if (!c) return;
    g_free(c->path);
    g_free(c->old_hex);
    g_free(c->new_hex);
    g_free(c);
}

static void set_status(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(a->status_label), msg);
    g_free(msg);
}

static gboolean db_exists_in(const char *root)
{
    char path[DB_MAX_PATH];
    if (db_path_for(root, path, sizeof(path)) != 0) return FALSE;
    return g_file_test(path, G_FILE_TEST_EXISTS);
}

static void update_root_label(App *a)
{
    gboolean present = db_exists_in(a->root);
    char *markup = g_markup_printf_escaped(
        "<b>Monitoring:</b> %s\n<small>Database (.syshash.db): %s</small>",
        a->root, present ? "present" : "not created yet");
    gtk_label_set_markup(GTK_LABEL(a->root_label), markup);
    g_free(markup);
}

static void set_root(App *a, const char *root)
{
    g_free(a->root);
    a->root = g_strdup(root);
    update_root_label(a);
}

static void set_busy(App *a, gboolean busy)
{
    gtk_widget_set_sensitive(a->btn_create, !busy);
    gtk_widget_set_sensitive(a->btn_open,   !busy);
    gtk_widget_set_sensitive(a->btn_verify, !busy);
    if (busy) gtk_widget_set_sensitive(a->apply_btn, FALSE);
}

/* ==========================================================================
 * Worker thread
 * ========================================================================== */

/* shared callback context for the streaming scan */
typedef struct {
    Job       *job;
    db_writer *writer;     /* create only                    */
    GPtrArray *scanned;    /* verify only: of ScanItem*      */
    int        failed;
} work_ctx;

typedef struct { char *path; char *hex; } ScanItem;

static void scanitem_free(gpointer p)
{
    ScanItem *s = p;
    if (!s) return;
    g_free(s->path);
    g_free(s->hex);
    g_free(s);
}

static int on_file(const char *path, const char hex[DB_HEX_LEN + 1], void *vctx)
{
    work_ctx *w = vctx;

    g_mutex_lock(&w->job->cur_lock);
    g_free(w->job->current);
    w->job->current = g_strdup(path);
    g_mutex_unlock(&w->job->cur_lock);

    if (w->writer) {
        if (db_writer_add(w->writer, path, hex) != 0) {
            w->failed = 1;
            return -1;
        }
    }
    if (w->scanned) {
        ScanItem *it = g_new(ScanItem, 1);
        it->path = g_strdup(path);
        it->hex  = g_strdup(hex);
        g_ptr_array_add(w->scanned, it);
    }

    g_atomic_int_inc(&w->job->done);
    return 0;
}

static void do_create(Job *job)
{
    g_atomic_int_set(&job->total, (gint)scan_count_files_at(job->root));

    db_writer *writer = db_writer_open_at(job->root);
    if (!writer) {
        job->ok = FALSE;
        job->message = g_strdup_printf("Failed to open database for writing in:\n%s", job->root);
        return;
    }

    work_ctx ctx = { job, writer, NULL, 0 };
    size_t hashed = 0, errors = 0;
    scan_stream_at(job->root, on_file, &ctx, &hashed, &errors);

    if (ctx.failed) {
        db_writer_close(writer);
        job->ok = FALSE;
        job->message = g_strdup("Write error while saving the database (disk full?).\n"
                                "The database is incomplete.");
        return;
    }
    if (db_writer_close(writer) != 0) {
        job->ok = FALSE;
        job->message = g_strdup("Failed to finalize the database file.");
        return;
    }

    job->errors = errors;
    job->ok = TRUE;
    if (errors)
        job->message = g_strdup_printf("Database created: %zu file%s hashed. "
                                       "(%zu unreadable file(s) skipped)",
                                       hashed, hashed == 1 ? "" : "s", errors);
    else
        job->message = g_strdup_printf("Database created: %zu file%s hashed.",
                                       hashed, hashed == 1 ? "" : "s");
}

static void do_verify(Job *job)
{
    db_t *db = db_load_at(job->root);
    if (!db) {
        job->ok = FALSE;
        job->message = g_strdup("No database found in this directory.\n"
                                "Use \"Create / Rebuild\" first.");
        return;
    }

    g_atomic_int_set(&job->total, (gint)scan_count_files_at(job->root));

    GPtrArray *scanned = g_ptr_array_new_with_free_func(scanitem_free);
    work_ctx ctx = { job, NULL, scanned, 0 };
    size_t hashed = 0, errors = 0;
    scan_stream_at(job->root, on_file, &ctx, &hashed, &errors);
    job->errors = errors;

    /* diff — same logic as the CLI */
    for (guint i = 0; i < scanned->len; i++) {
        ScanItem *it = g_ptr_array_index(scanned, i);
        db_entry *e = db_find(db, it->path);
        if (e) e->seen = 1;

        if (!e) {
            Change *c = g_new0(Change, 1);
            c->type = CHG_NEW; c->path = g_strdup(it->path); c->new_hex = g_strdup(it->hex);
            g_ptr_array_add(job->changes, c);
            job->n_new++;
        } else if (strcmp(e->hex, it->hex) != 0) {
            Change *c = g_new0(Change, 1);
            c->type = CHG_MOD; c->path = g_strdup(it->path);
            c->old_hex = g_strdup(e->hex); c->new_hex = g_strdup(it->hex);
            g_ptr_array_add(job->changes, c);
            job->n_mod++;
        } else {
            job->n_ok++;
        }
    }
    for (db_entry *e = db->head; e; e = e->next) {
        if (e->seen) continue;
        Change *c = g_new0(Change, 1);
        c->type = CHG_MISSING; c->path = g_strdup(e->path); c->old_hex = g_strdup(e->hex);
        g_ptr_array_add(job->changes, c);
        job->n_missing++;
    }

    g_ptr_array_free(scanned, TRUE);
    db_free(db);

    job->ok = TRUE;
    job->message = g_strdup_printf(
        "Unchanged: %zu   Modified: %zu   New: %zu   Missing: %zu",
        job->n_ok, job->n_mod, job->n_new, job->n_missing);
}

static gpointer worker_main(gpointer data)
{
    Job *job = data;
    if (job->kind == JOB_CREATE) do_create(job);
    else                         do_verify(job);
    g_atomic_int_set(&job->finished, 1);
    return NULL;
}

/* ==========================================================================
 * Completion + progress (main thread)
 * ========================================================================== */

static void job_free(Job *job)
{
    if (!job) return;
    g_free(job->root);
    g_free(job->message);
    g_free(job->current);
    g_mutex_clear(&job->cur_lock);
    if (job->changes) g_ptr_array_free(job->changes, TRUE);
    g_free(job);
}

static void show_info(App *a, GtkMessageType type, const char *text)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->window),
        GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", text);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void populate_changes(App *a, GPtrArray *changes)
{
    gtk_list_store_clear(a->store);
    for (guint i = 0; i < changes->len; i++) {
        Change *c = g_ptr_array_index(changes, i);
        const char *type = c->type == CHG_NEW ? "New"
                         : c->type == CHG_MOD ? "Modified" : "Missing";
        GtkTreeIter it;
        gtk_list_store_append(a->store, &it);
        gtk_list_store_set(a->store, &it,
            COL_ACCEPT, TRUE, COL_TYPE, type, COL_PATH, c->path, -1);
    }
}

static void on_job_done(App *a)
{
    Job *job = a->job;

    if (a->changes) { g_ptr_array_free(a->changes, TRUE); a->changes = NULL; }

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), job->ok ? 1.0 : 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->progress), job->ok ? "Done" : "Failed");

    if (job->kind == JOB_CREATE) {
        gtk_list_store_clear(a->store);
        gtk_widget_set_sensitive(a->apply_btn, FALSE);
        update_root_label(a);
        show_info(a, job->ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, job->message);
        set_status(a, "%s", job->message);
    } else { /* verify */
        if (!job->ok) {
            show_info(a, GTK_MESSAGE_WARNING, job->message);
            set_status(a, "Verification could not run.");
        } else {
            /* hand the changes array over to the app for later "apply" */
            a->changes = job->changes;
            job->changes = NULL;
            populate_changes(a, a->changes);
            gboolean has = a->changes->len > 0;
            gtk_widget_set_sensitive(a->apply_btn, has);
            set_status(a, "%s", job->message);
            if (!has)
                show_info(a, GTK_MESSAGE_INFO,
                          "All files intact. No tampering detected.");
            else if (job->errors)
                set_status(a, "%s  (%zu unreadable file(s) skipped)",
                           job->message, job->errors);
        }
    }

    set_busy(a, FALSE);
    job_free(job);
    a->job = NULL;
    a->worker = NULL;
}

static gboolean poll_worker(gpointer data)
{
    App *a = data;
    Job *job = a->job;
    if (!job) { a->poll_id = 0; return G_SOURCE_REMOVE; }

    gint total = g_atomic_int_get(&job->total);
    gint done  = g_atomic_int_get(&job->done);
    if (total > 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress),
                                      CLAMP((double)done / total, 0.0, 1.0));
        char buf[64];
        g_snprintf(buf, sizeof(buf), "%d / %d", done, total);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->progress), buf);
    } else {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(a->progress));
    }

    g_mutex_lock(&job->cur_lock);
    if (job->current) set_status(a, "Hashing: %s", job->current);
    g_mutex_unlock(&job->cur_lock);

    if (g_atomic_int_get(&job->finished)) {
        g_thread_join(a->worker);
        a->poll_id = 0;
        on_job_done(a);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void start_job(App *a, job_kind kind)
{
    if (a->worker) return;  /* already running */

    Job *job = g_new0(Job, 1);
    job->kind = kind;
    job->root = g_strdup(a->root);
    job->changes = g_ptr_array_new_with_free_func(change_free);
    g_mutex_init(&job->cur_lock);

    a->job = job;
    set_busy(a, TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->progress),
                              kind == JOB_CREATE ? "Building…" : "Verifying…");
    set_status(a, kind == JOB_CREATE ? "Counting files…" : "Scanning…");

    a->worker = g_thread_new("syshash-worker", worker_main, job);
    a->poll_id = g_timeout_add(80, poll_worker, a);
}

/* ==========================================================================
 * Button handlers
 * ========================================================================== */

static void on_create(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Choose a directory to protect", GTK_WINDOW(a->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Create here", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), a->root);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_widget_destroy(dlg);
        set_root(a, dir);
        g_free(dir);

        if (db_exists_in(a->root)) {
            GtkWidget *q = gtk_message_dialog_new(GTK_WINDOW(a->window),
                GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
                "A database already exists here and will be overwritten.\nRebuild it?");
            int r = gtk_dialog_run(GTK_DIALOG(q));
            gtk_widget_destroy(q);
            if (r != GTK_RESPONSE_OK) return;
        }
        start_job(a, JOB_CREATE);
    } else {
        gtk_widget_destroy(dlg);
    }
}

static void on_open(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open an existing .syshash.db", GTK_WINDOW(a->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);

    GtkFileChooser *ch = GTK_FILE_CHOOSER(dlg);
    gtk_file_chooser_set_filename(ch, a->root);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Syshash database (.syshash.db)");
    gtk_file_filter_add_pattern(f, DB_FILENAME);
    gtk_file_chooser_add_filter(ch, f);
    /* allow hidden files so .syshash.db is visible */
    gtk_file_chooser_set_show_hidden(ch, TRUE);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *file = gtk_file_chooser_get_filename(ch);
        gtk_widget_destroy(dlg);
        char *dir = g_path_get_dirname(file);
        set_root(a, dir);
        g_free(dir);
        g_free(file);
        gtk_list_store_clear(a->store);
        gtk_widget_set_sensitive(a->apply_btn, FALSE);
        set_status(a, "Opened database. Click \"Verify Integrity\" to check it.");
    } else {
        gtk_widget_destroy(dlg);
    }
}

static void on_verify(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    if (!db_exists_in(a->root)) {
        show_info(a, GTK_MESSAGE_WARNING,
                  "No database in this directory yet.\n"
                  "Use \"Create / Rebuild\" or \"Open Database…\" first.");
        return;
    }
    start_job(a, JOB_VERIFY);
}

static void on_accept_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell;
    App *a = data;
    GtkTreeIter it;
    GtkTreePath *p = gtk_tree_path_new_from_string(path_str);
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(a->store), &it, p)) {
        gboolean v;
        gtk_tree_model_get(GTK_TREE_MODEL(a->store), &it, COL_ACCEPT, &v, -1);
        gtk_list_store_set(a->store, &it, COL_ACCEPT, !v, -1);
    }
    gtk_tree_path_free(p);
}

static void on_apply(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    if (!a->changes || a->changes->len == 0) return;

    db_t *db = db_load_at(a->root);
    if (!db) { show_info(a, GTK_MESSAGE_ERROR, "Could not reload the database."); return; }

    /* walk the list store in parallel with the changes array (same order) */
    GtkTreeModel *m = GTK_TREE_MODEL(a->store);
    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(m, &it);
    size_t applied = 0;
    for (guint i = 0; i < a->changes->len && valid; i++) {
        Change *c = g_ptr_array_index(a->changes, i);
        gboolean accept = FALSE;
        gtk_tree_model_get(m, &it, COL_ACCEPT, &accept, -1);
        if (accept) {
            if (c->type == CHG_MISSING) db_remove(db, c->path);
            else                        db_upsert(db, c->path, c->new_hex);
            applied++;
        }
        valid = gtk_tree_model_iter_next(m, &it);
    }

    if (applied) {
        time_t t = time(NULL);
        struct tm *tm = gmtime(&t);
        strftime(db->updated, sizeof(db->updated), "%Y-%m-%dT%H:%M:%SZ", tm);
        if (db_save_at(a->root, db) != 0) {
            show_info(a, GTK_MESSAGE_ERROR, "Failed to write the updated database.");
            db_free(db);
            return;
        }
    }
    db_free(db);

    show_info(a, GTK_MESSAGE_INFO,
              applied ? "Database updated." : "No changes were accepted.");
    set_status(a, "%zu change(s) applied.", applied);

    /* clear state; database is now the new baseline */
    gtk_list_store_clear(a->store);
    g_ptr_array_free(a->changes, TRUE);
    a->changes = NULL;
    gtk_widget_set_sensitive(a->apply_btn, FALSE);
    update_root_label(a);
}

static void on_about(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    const char *authors[] = { AUTHOR, NULL };
    gtk_show_about_dialog(GTK_WINDOW(a->window),
        "program-name", "Syshash",
        "version", VERSION,
        "comments", "File Integrity Monitor using SHA3-512 hashing.",
        "authors", authors,
        "copyright", "© " AUTHOR,
        "logo-icon-name", "syshash",
        "license-type", GTK_LICENSE_MIT_X11,
        "website-label", "Syshash",
        NULL);
}

/* ==========================================================================
 * UI construction
 * ========================================================================== */

static GtkWidget *make_button(const char *label, const char *icon)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    if (icon) {
        GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
        gtk_button_set_image(GTK_BUTTON(b), img);
        gtk_button_set_always_show_image(GTK_BUTTON(b), TRUE);
    }
    return b;
}

static void activate(GtkApplication *gapp, gpointer data)
{
    App *a = data;
    a->app = gapp;
    a->root = g_strdup(g_get_home_dir());

    GtkWidget *win = gtk_application_window_new(gapp);
    a->window = win;
    gtk_window_set_title(GTK_WINDOW(win), "Syshash — File Integrity Monitor");
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 520);
    gtk_window_set_icon_name(GTK_WINDOW(win), "syshash");

    /* header bar with the About action */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Syshash");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "SHA3-512 integrity");
    GtkWidget *about = make_button("About", "help-about");
    g_signal_connect(about, "clicked", G_CALLBACK(on_about), a);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about);
    gtk_window_set_titlebar(GTK_WINDOW(win), header);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* root info */
    a->root_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(a->root_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(a->root_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(a->root_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), a->root_label, FALSE, FALSE, 0);

    /* action buttons */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    a->btn_create = make_button("Create / Rebuild…", "document-new");
    a->btn_open   = make_button("Open Database…",    "document-open");
    a->btn_verify = make_button("Verify Integrity",  "emblem-ok-symbolic");
    g_signal_connect(a->btn_create, "clicked", G_CALLBACK(on_create), a);
    g_signal_connect(a->btn_open,   "clicked", G_CALLBACK(on_open),   a);
    g_signal_connect(a->btn_verify, "clicked", G_CALLBACK(on_verify), a);
    gtk_box_pack_start(GTK_BOX(bar), a->btn_create, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), a->btn_open,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), a->btn_verify, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    /* changes list */
    a->store = gtk_list_store_new(N_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);
    a->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));

    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_accept_toggled), a);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->tree),
        gtk_tree_view_column_new_with_attributes("Accept", toggle, "active", COL_ACCEPT, NULL));

    GtkCellRenderer *txt = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->tree),
        gtk_tree_view_column_new_with_attributes("Status", txt, "text", COL_TYPE, NULL));
    GtkTreeViewColumn *pcol =
        gtk_tree_view_column_new_with_attributes("Path", gtk_cell_renderer_text_new(),
                                                 "text", COL_PATH, NULL);
    gtk_tree_view_column_set_expand(pcol, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->tree), pcol);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), a->tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    /* apply row */
    GtkWidget *apply_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    a->apply_btn = make_button("Accept selected changes", "document-save");
    gtk_widget_set_sensitive(a->apply_btn, FALSE);
    g_signal_connect(a->apply_btn, "clicked", G_CALLBACK(on_apply), a);
    gtk_box_pack_end(GTK_BOX(apply_row), a->apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), apply_row, FALSE, FALSE, 0);

    /* progress + status */
    a->progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(a->progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->progress), "Idle");
    gtk_box_pack_start(GTK_BOX(vbox), a->progress, FALSE, FALSE, 0);

    a->status_label = gtk_label_new("Ready.");
    gtk_label_set_xalign(GTK_LABEL(a->status_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(a->status_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(vbox), a->status_label, FALSE, FALSE, 0);

    update_root_label(a);
    gtk_widget_show_all(win);
}

int main(int argc, char **argv)
{
    App a;
    memset(&a, 0, sizeof(a));

    GtkApplication *app = gtk_application_new("org.syshash.gui",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &a);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    if (a.changes) g_ptr_array_free(a.changes, TRUE);
    g_free(a.root);
    g_object_unref(app);
    return status;
}
