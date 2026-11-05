/* MDA-8086 Kit Studio - Linux window (GTK 3)
 * The same program as the Windows one (main.c): editor, Assemble with kit check, Run on Kit,
 * kit command box, built-in examples and templates. Both use kitui.c and kitbuiltin.c.
 * Built for 64 and 32 bit Linux (Intel or AMD); needs GTK 3.10 or newer (2013 and later). */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "kitasm.h"
#include "kitcore.h"
#include "kitui.h"
#include "kitbuiltin.h"
#include "icon_png.h"

enum { JOB_NONE, JOB_RUN, JOB_CMD, JOB_FIND, JOB_PORT };
#define URI_TARGET 1001

/* ------------------------------------------------------------------ globals */
static GtkWidget *w_main, *w_edit, *w_out, *w_cmd, *w_port, *w_btnAsm, *w_btnRun, *w_btnStop,
                 *w_btnRefresh, *w_btnSend, *w_sbPos, *w_sbFile, *w_sbKit;
static GtkTextBuffer *b_edit, *b_out;
/* Simulator panel (right side) */
static GtkWidget *w_simBox, *w_simArea, *w_simPause, *w_speed, *w_speedLbl, *w_hpaned, *w_simCheck,
                 *w_floatCheck, *w_simWin;     /* w_simWin: the simulator in its own window */
static GtkTextTag *t_simLine;
static KitSimCtl g_sim;
static guint g_simTimer;
static gint64 g_simTick;
static int g_simMarked;            /* a line of the editor is marked (yellow) */
static guint g_simMarkIdle;
static const double SPEEDS[7] = { 1.0 / 1000, 1.0 / 300, 1.0 / 100, 1.0 / 30, 1.0 / 10, 1.0 / 3, 1.0 };
static GtkTextTag *t_kind[12];
static GtkTextMark *m_outEnd;
static GtkAccelGroup *g_accel;
static int g_outAtLineStart = 1, g_fillingPorts, g_closing;

static char g_path[4096];               /* current file, "" = not saved yet */
static char g_shownName[256];           /* name shown for an unsaved copy of an example */
static char g_origin[64];               /* "Template 1": a new program made from a template */
static char g_tmpdir[4096];             /* private folder for the assembler's files */
static char g_logPath[4096];            /* kit log: every byte to/from the kit */

/* worker thread: all talking to the kit happens there */
static GMutex g_mx;
static GCond g_cv;
static int g_job, g_jobGo, g_busy, g_quit;
static char *g_jobText;
static char g_jobPort[KP_NAMELEN];
static volatile long g_cancel;
static KitLink g_kit;
static GThread *g_thread;

/* messages from the worker, shown in order by the main loop */
typedef struct { int state, kind, busy; char *text, *port; } UiMsg;
static GMutex g_qmx;
static GQueue g_queue = G_QUEUE_INIT;
static int g_qpending;

/* undo / redo for the editor */
typedef struct { int insert, pos, nchars; char *text; gint64 when; } UndoAct;
static GPtrArray *g_undo, *g_redo;
static int g_undoBusy;

static void out_msg(int kind, const char *text);
static void outf(int kind, const char *fmt, ...);
static void set_busy_ui(int busy, const char *port);

/* ------------------------------------------------------------------ output pane */
static const char *kind_color(int kind)
{
    switch (kind) {
    case KO_OK:    return "#008000";
    case KO_WARN:  return "#B96400";
    case KO_ERR:   return "#CD0000";
    case KO_KIT:   return "#0046A5";
    case KO_DUMP:  return "#6E6E6E";   /* machine code dump */
    case KO_HINT:  return "#7832A0";   /* hints for assembler errors */
    case KO_CHECK: return "#006978";   /* kit check (model of the kit) */
    default:       return "#282828";
    }
}

static void out_raw(int kind, const char *text)
{
    GtkTextIter end;
    char *fixed = NULL;
    size_t n;
    if (gtk_text_buffer_get_char_count(b_out) > 400000) {     /* keep the pane small */
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(b_out, &a, 0);
        gtk_text_buffer_get_iter_at_offset(b_out, &b, 200000);
        gtk_text_buffer_delete(b_out, &a, &b);
    }
    if (!g_utf8_validate(text, -1, NULL)) {                   /* GTK needs UTF-8 */
        fixed = g_convert(text, -1, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
        if (fixed) text = fixed;
    }
    gtk_text_buffer_get_end_iter(b_out, &end);
    gtk_text_buffer_insert_with_tags(b_out, &end, text, -1, t_kind[kind >= 0 && kind < 12 ? kind : 0], NULL);
    n = strlen(text);
    if (n) g_outAtLineStart = text[n - 1] == '\n';
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(w_out), m_outEnd, 0.0, FALSE, 0, 0);
    g_free(fixed);
}

/* messages always start on a new line; kit text is shown exactly as it comes */
static void out_msg(int kind, const char *text)
{
    if (kind != KO_KIT) {
        while (*text == '\n') text++;
        if (!g_outAtLineStart) out_raw(KO_INFO, "\n");
        out_raw(kind, text);
        if (!g_outAtLineStart) out_raw(kind, "\n");
    } else {
        out_raw(kind, text);
    }
}

static void outf(int kind, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out_msg(kind, buf);
}

static void ui_out(int kind, const char *text) { out_msg(kind, text); }

/* ------------------------------------------------------------------ worker messages */
static gboolean drain_queue(gpointer data)
{
    (void)data;
    for (;;) {
        UiMsg *m;
        g_mutex_lock(&g_qmx);
        m = (UiMsg *)g_queue_pop_head(&g_queue);
        if (!m) g_qpending = 0;
        g_mutex_unlock(&g_qmx);
        if (!m) break;
        if (!g_closing) {
            if (m->state) set_busy_ui(m->busy, m->port);
            else out_msg(m->kind, m->text);
        }
        g_free(m->text);
        g_free(m->port);
        g_free(m);
    }
    return FALSE;
}

static void queue_push(UiMsg *m)
{
    g_mutex_lock(&g_qmx);
    g_queue_push_tail(&g_queue, m);
    if (!g_qpending) { g_qpending = 1; g_idle_add(drain_queue, NULL); }
    g_mutex_unlock(&g_qmx);
}

/* called from the worker thread */
static void worker_out(void *ctx, int kind, const char *text)
{
    UiMsg *m = g_new0(UiMsg, 1);
    (void)ctx;
    m->kind = kind;
    m->text = g_strdup(text);
    queue_push(m);
}

static void post_state(void)
{
    UiMsg *m = g_new0(UiMsg, 1);
    m->state = 1;
    g_mutex_lock(&g_mx);
    m->busy = g_busy;
    g_mutex_unlock(&g_mx);
    m->port = g_kit.port ? g_strdup(kp_name(g_kit.port)) : NULL;
    queue_push(m);
}

/* ------------------------------------------------------------------ worker thread */
/* kit log: appended each session, started again when it is over 1 MB */
static FILE *open_kit_log(void)
{
    struct stat st;
    GDateTime *now;
    gchar *stamp;
    FILE *f;
    int big = stat(g_logPath, &st) == 0 && st.st_size > 1000000;
    f = fopen(g_logPath, big ? "w" : "a");
    if (!f) return NULL;
    now = g_date_time_new_now_local();
    stamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    fprintf(f, "\n===== " APP_NAME " " APP_VERSION " (Linux)   %s =====\n", stamp ? stamp : "");
    g_free(stamp);
    g_date_time_unref(now);
    fflush(f);
    return f;
}

static void set_busy(int busy)
{
    g_mutex_lock(&g_mx);
    g_busy = busy;
    g_mutex_unlock(&g_mx);
}

static gpointer worker(gpointer arg)
{
    FILE *log;
    (void)arg;
    kit_init(&g_kit, worker_out, NULL, &g_cancel);
    log = open_kit_log();
    if (log) kit_set_log(&g_kit, log);
    for (;;) {
        int job, go;
        char *text;
        g_mutex_lock(&g_mx);
        if (g_job == JOB_NONE && !g_quit) {
            gint64 end = g_get_monotonic_time() + (g_kit.port ? 30 : 300) * G_TIME_SPAN_MILLISECOND;
            g_cond_wait_until(&g_cv, &g_mx, end);
        }
        if (g_quit) { g_mutex_unlock(&g_mx); break; }
        job = g_job; text = g_jobText; go = g_jobGo;
        if (job == JOB_PORT) snprintf(g_kit.want, sizeof g_kit.want, "%s", g_jobPort);
        g_job = JOB_NONE; g_jobText = NULL;
        if (job != JOB_NONE) g_busy = 1;
        g_mutex_unlock(&g_mx);

        if (job == JOB_NONE) {
            if (g_kit.port && kit_poll(&g_kit) < 0) post_state();
            continue;
        }
        __atomic_store_n(&g_cancel, 0, __ATOMIC_SEQ_CST);
        post_state();
        switch (job) {
        case JOB_PORT:
            kit_disconnect(&g_kit);
            break;
        case JOB_FIND:
            kit_find(&g_kit, 1);
            break;
        case JOB_CMD:          /* make sure the monitor is at its prompt first */
            if (kit_find(&g_kit, 1)) kit_command(&g_kit, text);
            break;
        case JOB_RUN: {
            int n = 0;
            if (kit_find(&g_kit, 1) && kit_download(&g_kit, text, &n) && go)
                kit_go(&g_kit, 0, 0x1000);
            break;
        }
        }
        free(text);
        set_busy(0);
        post_state();
    }
    kit_disconnect(&g_kit);
    g_kit.log = NULL;
    if (log) fclose(log);
    return NULL;
}

static int start_job(int job, const char *text, int go)
{
    int ok = 0;
    g_mutex_lock(&g_mx);
    if (!g_busy && g_job == JOB_NONE) {
        g_job = job;
        g_jobText = text ? strdup(text) : NULL;
        g_jobGo = go;
        ok = 1;
        g_cond_signal(&g_cv);
    }
    g_mutex_unlock(&g_mx);
    if (!ok) gtk_widget_error_bell(w_main);
    return ok;
}

static int is_busy(void)
{
    int b;
    g_mutex_lock(&g_mx);
    b = g_busy || g_job != JOB_NONE;
    g_mutex_unlock(&g_mx);
    return b;
}

/* ------------------------------------------------------------------ dialogs */
static void msg_box(GtkMessageType type, const char *fmt, ...)
{
    GtkWidget *d;
    char buf[4600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    d = gtk_message_dialog_new(GTK_WINDOW(w_main), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                               type, GTK_BUTTONS_OK, "%s", buf);
    gtk_window_set_title(GTK_WINDOW(d), APP_NAME);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* a help text in a fixed-width font, as on Windows */
static void show_text_box(const char *title, const char *text)
{
    GtkWidget *d, *label, *area;
    gchar *esc = g_markup_escape_text(text, -1);
    gchar *markup = g_strdup_printf("<tt>%s</tt>", esc);
    d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w_main),
                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                    "_OK", GTK_RESPONSE_OK, NULL);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_pack_start(GTK_BOX(area), label, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(markup);
    g_free(esc);
}

/* ------------------------------------------------------------------ files */
/* file text with '\n' line ends and valid UTF-8 (NUL bytes become spaces) */
static char *read_text_file(const char *path)
{
    gchar *raw = NULL, *out, *q;
    gsize n = 0, i;
    if (!g_file_get_contents(path, &raw, &n, NULL)) return NULL;
    if (n > 4 * 1024 * 1024) { g_free(raw); return NULL; }
    out = (gchar *)g_malloc(n + 1);
    for (i = 0, q = out; i < n; i++) {
        char c = raw[i];
        if (c == 0) c = ' ';
        if (c == '\r') {
            *q++ = '\n';
            if (i + 1 < n && raw[i + 1] == '\n') i++;
        } else {
            *q++ = c;
        }
    }
    *q = 0;
    g_free(raw);
    if (!g_utf8_validate(out, -1, NULL)) {      /* e.g. a file from an old Windows editor */
        gchar *u = g_convert(out, -1, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
        if (u) { g_free(out); out = u; }
    }
    return out;
}

/* written with CR LF line ends, like the lab's DOS/Windows tools expect */
static int write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    const char *p;
    if (!f) return 0;
    for (p = text; *p; p++) {
        if (*p == '\n' && fputc('\r', f) == EOF) { fclose(f); return 0; }
        if (fputc(*p, f) == EOF) { fclose(f); return 0; }
    }
    return fclose(f) == 0;
}

static char *editor_text(void)
{
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(b_edit, &a, &b);
    return gtk_text_buffer_get_text(b_edit, &a, &b, FALSE);
}

static const char *shown_name(void)
{
    if (g_path[0]) return kitui_file_part(g_path);
    if (g_shownName[0]) return g_shownName;
    return "Untitled";
}

static void update_title(void)
{
    char t[4200], s[4300];
    int mod = gtk_text_buffer_get_modified(b_edit);
    snprintf(t, sizeof t, "%s%s - %s", shown_name(), mod ? "*" : "", APP_NAME);
    gtk_window_set_title(GTK_WINDOW(w_main), t);
    if (g_path[0])
        snprintf(s, sizeof s, "%s%s", g_path, mod ? "  (changed)" : "");
    else if (g_shownName[0])
        snprintf(s, sizeof s, "%s (copy of an example, not saved yet)%s", g_shownName, mod ? "  (changed)" : "");
    else if (g_origin[0])
        snprintf(s, sizeof s, "Untitled (new program from %s, not saved yet)%s", g_origin, mod ? "  (changed)" : "");
    else
        snprintf(s, sizeof s, "Untitled (not saved yet)%s", mod ? "  (changed)" : "");
    gtk_label_set_text(GTK_LABEL(w_sbFile), s);
}

static void undo_free_act(gpointer p)
{
    UndoAct *a = (UndoAct *)p;
    if (!a) return;
    g_free(a->text);
    g_free(a);
}

static void undo_clear(void)
{
    g_ptr_array_set_size(g_undo, 0);
    g_ptr_array_set_size(g_redo, 0);
}

/* path = file on disk (or NULL); name = name shown for an unsaved copy of an example (or NULL);
   origin = "Template 1" for a new program made from a template (or NULL) */
static void set_editor_text(const char *text, const char *path, const char *name, const char *origin)
{
    GtkTextIter start;
    g_undoBusy = 1;
    gtk_text_buffer_set_text(b_edit, text, -1);
    g_undoBusy = 0;
    undo_clear();
    gtk_text_buffer_get_start_iter(b_edit, &start);
    gtk_text_buffer_place_cursor(b_edit, &start);
    gtk_text_buffer_set_modified(b_edit, FALSE);
    snprintf(g_path, sizeof g_path, "%s", path ? path : "");
    snprintf(g_shownName, sizeof g_shownName, "%s", (!path && name) ? name : "");
    snprintf(g_origin, sizeof g_origin, "%s", (!path && origin) ? origin : "");
    update_title();
}

static int save_to(const char *path)
{
    char *text = editor_text();
    int ok = text && write_text_file(path, text);
    g_free(text);
    if (!ok) {
        msg_box(GTK_MESSAGE_ERROR, "Could not save the file:\n%s\n\nTry File > Save As in another folder "
                "(e.g. your home folder).", path);
        return 0;
    }
    if (path != g_path) snprintf(g_path, sizeof g_path, "%s", path);   /* (never copy onto itself) */
    g_shownName[0] = 0;
    g_origin[0] = 0;
    gtk_text_buffer_set_modified(b_edit, FALSE);
    update_title();
    return 1;
}

static void add_filter(GtkWidget *chooser, const char *name, const char *pattern)
{
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, name);
    gtk_file_filter_add_pattern(f, pattern);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), f);
}

static int save_as(void)
{
    GtkWidget *d;
    int ok = 0;
    d = gtk_file_chooser_dialog_new("Save program", GTK_WINDOW(w_main), GTK_FILE_CHOOSER_ACTION_SAVE,
                                    "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d), TRUE);
    add_filter(d, "Assembly program (*.asm)", "*.[aA][sS][mM]");
    add_filter(d, "All files", "*");
    if (g_path[0]) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(d), g_path);
    } else {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), g_get_home_dir());
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), g_shownName[0] ? g_shownName : "PROGRAM.ASM");
    }
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (file) {
            char path[4200];
            const char *name = kitui_file_part(file);
            snprintf(path, sizeof path, "%s%s", file, strchr(name, '.') ? "" : ".asm");
            gtk_widget_destroy(d);
            d = NULL;
            ok = save_to(path);
            g_free(file);
        }
    }
    if (d) gtk_widget_destroy(d);
    return ok;
}

static int do_save(void)
{
    if (!g_path[0]) return save_as();
    return save_to(g_path);
}

/* returns 0 if the user cancelled */
static int ask_save_changes(void)
{
    GtkWidget *d;
    int r;
    if (!gtk_text_buffer_get_modified(b_edit)) return 1;
    d = gtk_message_dialog_new(GTK_WINDOW(w_main), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                               GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                               "The program was changed. Save it first?");
    gtk_window_set_title(GTK_WINDOW(d), APP_NAME);
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, "_No", GTK_RESPONSE_NO,
                           "_Yes", GTK_RESPONSE_YES, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_YES);
    r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_YES) return do_save();
    return r == GTK_RESPONSE_NO;
}

static void open_path(const char *path)
{
    char *text = read_text_file(path);
    if (!text) {
        msg_box(GTK_MESSAGE_ERROR, "Could not open:\n%s", path);
        return;
    }
    set_editor_text(text, path, NULL, NULL);
    g_free(text);
    outf(KO_INFO, "Opened %s", path);
}

static char *choose_file(const char *title, const char *fname, const char *fpattern)
{
    GtkWidget *d = gtk_file_chooser_dialog_new(title, GTK_WINDOW(w_main), GTK_FILE_CHOOSER_ACTION_OPEN,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    char *file = NULL;
    add_filter(d, fname, fpattern);
    add_filter(d, "All files", "*");
    if (g_path[0]) {
        char *dir = g_path_get_dirname(g_path);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), dir);
        g_free(dir);
    }
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
        file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
    gtk_widget_destroy(d);
    return file;
}

static void do_open(void)
{
    char *file;
    if (!ask_save_changes()) return;
    file = choose_file("Open program", "Assembly program (*.asm)", "*.[aA][sS][mM]");
    if (file) { open_path(file); g_free(file); }
}

/* ------------------------------------------------------------------ built-in examples, templates */
static void load_builtin(const KitBuiltin *b, int is_template)
{
    char origin[64], *dash;
    if (!ask_save_changes()) return;
    snprintf(origin, sizeof origin, "%s", b->menu);
    dash = strstr(origin, " - ");               /* "Template 1 - Short form ..." -> "Template 1" */
    if (dash) *dash = 0;
    set_editor_text(b->text, NULL, is_template ? NULL : b->name, is_template ? origin : NULL);
    if (is_template)
        outf(KO_INFO, "New program from %s: write your program where the comments say, "
             "then File > Save As.", origin);
    else
        outf(KO_INFO, "Opened example %s (a copy: File > Save As keeps your own version).", b->name);
}

static void on_example(GtkMenuItem *it, gpointer data)
{
    (void)it;
    load_builtin(&kit_examples[GPOINTER_TO_INT(data)], 0);
}

static void on_template(GtkMenuItem *it, gpointer data)
{
    (void)it;
    load_builtin(&kit_templates[GPOINTER_TO_INT(data)], 1);
}

/* ------------------------------------------------------------------ assemble, run */
/* assembles the editor text; on success returns malloc'd .ABS text */
static char *assemble_ex(int quiet_dump, KitUiProgram *prog)
{
    char src[4200];
    char *text, *abs;
    if (g_path[0]) {
        if (!save_to(g_path)) return NULL;
        snprintf(src, sizeof src, "%s", g_path);
    } else {
        text = editor_text();
        snprintf(src, sizeof src, "%sUNTITLED.ASM", g_tmpdir);
        if (!text || !write_text_file(src, text)) {
            g_free(text);
            outf(KO_ERR, "Cannot write a temporary file in %s", g_tmpdir);
            return NULL;
        }
        g_free(text);
    }
    if (!g_outAtLineStart) out_raw(KO_INFO, "\n");
    outf(KO_INFO, "---- Assembling %s ----", g_path[0] ? kitui_file_part(g_path) :
         (g_shownName[0] ? g_shownName : "(not saved yet)"));
    text = editor_text();
    abs = kitui_assemble(src, g_path[0] ? kitui_file_part(g_path) : (g_shownName[0] ? g_shownName : "UNTITLED.ASM"),
                         text, g_tmpdir, g_path[0] ? g_path : NULL, quiet_dump, ui_out, prog);
    g_free(text);
    return abs;
}

static char *assemble(int quiet_dump) { return assemble_ex(quiet_dump, NULL); }

/* ------------------------------------------------------------------ simulator panel */
static void cd_rgb(cairo_t *cr, unsigned c)
{
    cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}

static void cd_rect(void *ctx, double x, double y, double w, double h, double r, unsigned fill, unsigned line)
{
    cairo_t *cr = (cairo_t *)ctx;
    if (line != KV_NONE) { x += 0.5; y += 0.5; w -= 1; h -= 1; }     /* sharp 1-pixel outlines */
    if (r > 0.5) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
        cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
        cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
        cairo_close_path(cr);
    } else {
        cairo_rectangle(cr, x, y, w, h);
    }
    if (fill != KV_NONE) {
        cd_rgb(cr, fill);
        if (line != KV_NONE) cairo_fill_preserve(cr); else cairo_fill(cr);
    }
    if (line != KV_NONE) {
        cd_rgb(cr, line);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }
}

static void cd_circle(void *ctx, double cx, double cy, double r, unsigned fill, unsigned line, double width)
{
    cairo_t *cr = (cairo_t *)ctx;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    if (fill != KV_NONE) {
        cd_rgb(cr, fill);
        if (line != KV_NONE) cairo_fill_preserve(cr); else cairo_fill(cr);
    }
    if (line != KV_NONE) {
        cd_rgb(cr, line);
        cairo_set_line_width(cr, width);
        cairo_stroke(cr);
    }
}

static void cd_poly(void *ctx, const double *xy, int n, unsigned fill)
{
    cairo_t *cr = (cairo_t *)ctx;
    int i;
    cairo_new_path(cr);
    cairo_move_to(cr, xy[0], xy[1]);
    for (i = 1; i < n; i++) cairo_line_to(cr, xy[2 * i], xy[2 * i + 1]);
    cairo_close_path(cr);
    cd_rgb(cr, fill);
    cairo_fill(cr);
}

static void cd_text(void *ctx, double x, double y, const char *str, double size, int flags, unsigned color)
{
    cairo_t *cr = (cairo_t *)ctx;
    cairo_font_extents_t fe;
    cairo_select_font_face(cr, (flags & KV_MONO) ? "Monospace" : "Sans", CAIRO_FONT_SLANT_NORMAL,
                           (flags & KV_BOLD) ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_font_extents(cr, &fe);
    if (flags & (KV_CENTER | KV_RIGHT)) {
        cairo_text_extents_t te;
        cairo_text_extents(cr, str, &te);
        x -= (flags & KV_CENTER) ? te.x_advance / 2 : te.x_advance;
    }
    cd_rgb(cr, color);
    cairo_move_to(cr, x, y + fe.ascent);
    cairo_show_text(cr, str);
}

static gboolean on_sim_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    KitDraw kd;
    (void)data;
    kd.ctx = cr;
    kd.rect = cd_rect;
    kd.circle = cd_circle;
    kd.poly = cd_poly;
    kd.text = cd_text;
    kitview_draw(&kd, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w), &g_sim.panel);
    return FALSE;
}

/* refresh the panel; when the program does not run, mark the next line in the editor */
static void sim_show(void)
{
    GtkTextIter a, b;
    kitsim_update(&g_sim);
    gtk_widget_queue_draw(w_simArea);
    gtk_widget_set_sensitive(w_simPause, g_sim.panel.running);
    gtk_text_buffer_get_bounds(b_edit, &a, &b);
    gtk_text_buffer_remove_tag(b_edit, t_simLine, &a, &b);
    g_simMarked = 0;
    if (!g_sim.panel.running && g_sim.panel.line > 0 && g_sim.panel.line <= gtk_text_buffer_get_line_count(b_edit)) {
        gtk_text_buffer_get_iter_at_line(b_edit, &a, g_sim.panel.line - 1);
        b = a;
        if (!gtk_text_iter_ends_line(&b)) gtk_text_iter_forward_to_line_end(&b);
        gtk_text_iter_forward_char(&b);
        gtk_text_buffer_apply_tag(b_edit, t_simLine, &a, &b);
        g_simMarked = 1;
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(w_edit), &a, 0.15, FALSE, 0, 0);
    }
}

/* the editor's program in the simulator: assembled again only when the text changed */
static int sim_prepare(int *fresh)
{
    char *text = editor_text(), *abs;
    KitUiProgram prog;
    *fresh = 0;
    if (text && kitsim_same_source(&g_sim, text)) { g_free(text); return 1; }
    g_free(text);
    memset(&prog, 0, sizeof prog);
    abs = assemble_ex(1, &prog);
    if (!abs) { kitui_free_program(&prog); return 0; }
    free(abs);
    text = editor_text();
    kitsim_load(&g_sim, &prog, text ? text : "");
    g_free(text);
    outf(KO_INFO, "Simulator: program loaded (%d bytes at 0000:1000).", g_sim.prog.len);
    *fresh = 1;
    return 1;
}

static gboolean sim_tick(gpointer data)
{
    gint64 now = g_get_monotonic_time();
    (void)data;
    if (g_closing) { g_simTimer = 0; return FALSE; }
    kitsim_tick(&g_sim, (now - g_simTick) / 1000000.0);
    g_simTick = now;
    sim_show();
    if (!g_sim.panel.running) { g_simTimer = 0; return FALSE; }
    return TRUE;
}

static void sim_stop_timer(void)
{
    if (g_simTimer) { g_source_remove(g_simTimer); g_simTimer = 0; }
}

static void act_simrun(void)
{
    int fresh;
    if (!sim_prepare(&fresh)) return;
    if (g_sim.panel.v.stop >= 0) kitsim_reset(&g_sim);     /* it had stopped: start again */
    g_sim.panel.running = 1;
    g_simTick = g_get_monotonic_time();
    if (!g_simTimer) g_simTimer = g_timeout_add(30, sim_tick, NULL);
    sim_show();
}

static void act_simstep(void)
{
    int fresh;
    if (!sim_prepare(&fresh)) return;
    sim_stop_timer();
    g_sim.panel.running = 0;
    if (!fresh) {                       /* just loaded: first show where it starts */
        if (g_sim.panel.v.stop >= 0) kitsim_reset(&g_sim);
        else kitsim_step(&g_sim);
    }
    sim_show();
}

static void act_simpause(void)
{
    if (!g_sim.panel.running) return;
    g_sim.panel.running = 0;
    sim_stop_timer();
    sim_show();
}

static void act_simreset(void)
{
    sim_stop_timer();
    kitsim_reset(&g_sim);
    sim_show();
}

static void on_speed(GtkRange *r, gpointer data)
{
    char buf[40];
    int i = (int)(gtk_range_get_value(r) + 0.5);
    (void)data;
    if (i < 0) i = 0;
    if (i > 6) i = 6;
    g_sim.panel.speed = SPEEDS[i];
    kitview_speed_text(SPEEDS[i], buf, sizeof buf);
    gtk_label_set_text(GTK_LABEL(w_speedLbl), buf);
    gtk_widget_queue_draw(w_simArea);
}

/* Simulator > Separate window: the panel in its own window (like emu8086), for small screens */
static gboolean on_simwin_delete(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w; (void)ev; (void)data;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w_floatCheck), FALSE);    /* back to the main window */
    return TRUE;
}

static void sim_set_float(int on)
{
    if (on == (w_simWin != NULL)) return;
    g_object_ref(w_simBox);
    gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(w_simBox)), w_simBox);
    if (on) {
        GdkRectangle wa;
        GdkScreen *scr = gdk_screen_get_default();
        int W = 600, H = 720;
        gdk_screen_get_monitor_workarea(scr, gdk_screen_get_primary_monitor(scr), &wa);
        if (wa.height > 200 && H > wa.height - 30) H = wa.height - 30;
        if (wa.width > 200 && W > wa.width - 40) W = wa.width - 40;
        w_simWin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(w_simWin), "Simulator - " APP_NAME);
        gtk_window_set_transient_for(GTK_WINDOW(w_simWin), GTK_WINDOW(w_main));   /* stays above the editor */
        gtk_window_set_default_size(GTK_WINDOW(w_simWin), W, H);
        gtk_window_add_accel_group(GTK_WINDOW(w_simWin), g_accel);                 /* F6, F8, F9 work here too */
        g_signal_connect(w_simWin, "delete-event", G_CALLBACK(on_simwin_delete), NULL);
        gtk_container_add(GTK_CONTAINER(w_simWin), w_simBox);
        if (wa.width > 200) gtk_window_move(GTK_WINDOW(w_simWin), wa.x + wa.width - W - 4, wa.y + 4);
        gtk_widget_show_all(w_simWin);
        on_speed(GTK_RANGE(w_speed), NULL);
    } else {
        gtk_paned_pack2(GTK_PANED(w_hpaned), w_simBox, TRUE, FALSE);
        gtk_widget_destroy(w_simWin);
        w_simWin = NULL;
    }
    g_object_unref(w_simBox);
}

static void on_sim_float(GtkCheckMenuItem *it, gpointer data)
{
    int on = gtk_check_menu_item_get_active(it);
    (void)data;
    if (on && !gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w_simCheck)))
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w_simCheck), TRUE);
    sim_set_float(on);
}

static void on_sim_panel(GtkCheckMenuItem *it, gpointer data)
{
    int on = gtk_check_menu_item_get_active(it);
    (void)data;
    if (!on && w_simWin) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w_floatCheck), FALSE);
    gtk_widget_set_visible(w_simBox, on);
}

static void do_assemble(void)
{
    char *a = assemble(0);
    free(a);
}

static void do_run(void)
{
    char *abs;
    if (is_busy()) { gtk_widget_error_bell(w_main); return; }
    abs = assemble(1);
    if (!abs) return;
    start_job(JOB_RUN, abs, 1);
    free(abs);
}

static void send_abs_file(const char *file)
{
    char *text = read_text_file(file), why[200];
    int n = 0, recs;
    if (!text) { outf(KO_ERR, "Could not read %s", file); return; }
    recs = kit_check_abs(text, &n, why, sizeof why);
    if (recs <= 0) {
        outf(KO_ERR, "%s: %s", kitui_file_part(file), recs < 0 ? why : "no program bytes in this file");
    } else {
        outf(KO_INFO, "---- Sending %s (%d bytes) ----", kitui_file_part(file), n);
        start_job(JOB_RUN, text, 1);
    }
    g_free(text);
}

static void do_send_abs(void)
{
    char *file;
    if (is_busy()) { gtk_widget_error_bell(w_main); return; }
    file = choose_file("Send .ABS file to the kit", "Kit program (*.abs)", "*.[aA][bB][sS]");
    if (file) { send_abs_file(file); g_free(file); }
}

static void do_stop(void)
{
    if (is_busy()) {
        __atomic_store_n(&g_cancel, 1, __ATOMIC_SEQ_CST);
        outf(KO_WARN, "Stopping ...");
    }
}

static void do_send_cmd(void)
{
    const char *line = gtk_entry_get_text(GTK_ENTRY(w_cmd));
    if (line[0] && start_job(JOB_CMD, line, 0)) gtk_entry_set_text(GTK_ENTRY(w_cmd), "");
    gtk_widget_grab_focus(w_cmd);
}

/* jump to the line number in an assembler message or hint */
static void goto_line(int num)
{
    GtkTextIter s, e;
    if (num <= 0 || num > gtk_text_buffer_get_line_count(b_edit)) return;
    gtk_text_buffer_get_iter_at_line(b_edit, &s, num - 1);
    e = s;
    if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
    gtk_text_buffer_select_range(b_edit, &e, &s);
    gtk_widget_grab_focus(w_edit);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(w_edit), gtk_text_buffer_get_insert(b_edit), 0.2, FALSE, 0, 0);
}

static gboolean on_out_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)data;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        GtkTextWindowType wt = gtk_text_view_get_window_type(GTK_TEXT_VIEW(w), ev->window);
        GtkTextIter it, s, e;
        gint bx, by, num;
        gchar *line;
        if (wt == GTK_TEXT_WINDOW_PRIVATE) wt = GTK_TEXT_WINDOW_WIDGET;
        gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(w), wt, (gint)ev->x, (gint)ev->y, &bx, &by);
        gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(w), &it, bx, by);
        s = it;
        gtk_text_iter_set_line_offset(&s, 0);
        e = s;
        if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
        line = gtk_text_buffer_get_text(b_out, &s, &e, FALSE);
        num = kitui_error_line(line);
        g_free(line);
        if (num > 0) { goto_line(num); return TRUE; }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ ports */
static void fill_ports(void)
{
    char names[KP_MAXPORTS][KP_NAMELEN];
    gchar *cur = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(w_port));
    int n, i, sel = 0;
    n = kp_list(names, KP_MAXPORTS);
    g_fillingPorts = 1;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(w_port));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w_port), "Auto (find kit)");
    for (i = 0; i < n; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w_port), names[i]);
        if (cur && strcmp(cur, names[i]) == 0) sel = i + 1;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(w_port), sel);
    g_fillingPorts = 0;
    g_free(cur);
    if (n == 0) {
        outf(KO_WARN, "No serial port found on this PC. Connect the kit with a USB-to-RS232 adapter "
             "(or a COM port), then press Refresh.");
        return;
    }
    {
        char list[KP_MAXPORTS * (KP_NAMELEN + 2)] = "";
        for (i = 0; i < n; i++) { if (i) strcat(list, ", "); strcat(list, names[i]); }
        outf(KO_INFO, "Serial ports on this PC: %s", list);
    }
    for (i = 0; i < n; i++) {           /* the most common problem on Linux: no permission */
        if (access(names[i], R_OK | W_OK) != 0 && errno == EACCES) {
            outf(KO_WARN, "No permission to use %s. Run this once in a terminal, then log out "
                 "and log in again:\n    sudo usermod -aG dialout $USER", names[i]);
            break;
        }
    }
}

static void on_port_changed(GtkComboBox *c, gpointer data)
{
    gchar *name;
    (void)data;
    if (g_fillingPorts) return;
    name = gtk_combo_box_get_active(c) > 0 ? gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c)) : NULL;
    g_mutex_lock(&g_mx);
    snprintf(g_jobPort, sizeof g_jobPort, "%s", name ? name : "");
    g_mutex_unlock(&g_mx);
    if (!start_job(JOB_PORT, NULL, 0))
        outf(KO_WARN, "Busy - the port change will apply after the current job.");
    else
        outf(KO_INFO, "Port: %s", name ? name : "Auto (find the kit on any serial port)");
    g_free(name);
}

static void set_busy_ui(int busy, const char *port)
{
    char buf[200];
    gtk_widget_set_sensitive(w_btnRun, !busy);
    gtk_widget_set_sensitive(w_btnSend, !busy);
    gtk_widget_set_sensitive(w_btnStop, busy);
    gtk_widget_set_sensitive(w_port, !busy);
    gtk_widget_set_sensitive(w_btnRefresh, !busy);
    if (busy) snprintf(buf, sizeof buf, "Kit: working...  (Stop to cancel)");
    else if (port) snprintf(buf, sizeof buf, "Kit: connected on %s", port);
    else snprintf(buf, sizeof buf, "Kit: not connected");
    gtk_label_set_text(GTK_LABEL(w_sbKit), buf);
}

/* ------------------------------------------------------------------ editor: undo, position */
static UndoAct *last_act(GPtrArray *a) { return a->len ? (UndoAct *)g_ptr_array_index(a, a->len - 1) : NULL; }

static void on_insert_text(GtkTextBuffer *b, GtkTextIter *loc, gchar *text, gint len, gpointer data)
{
    UndoAct *a, *l;
    glong nchars = g_utf8_strlen(text, len);
    gint64 now = g_get_monotonic_time();
    (void)b; (void)data;
    if (g_undoBusy) return;
    g_ptr_array_set_size(g_redo, 0);
    l = last_act(g_undo);
    /* typing letters one after another is one undo step */
    if (l && l->insert && nchars == 1 && text[0] != '\n' && l->pos + l->nchars == gtk_text_iter_get_offset(loc) &&
        now - l->when < 2000000 && l->text[strlen(l->text) - 1] != '\n') {
        gchar *t = g_strdup_printf("%s%.*s", l->text, len, text);
        g_free(l->text);
        l->text = t;
        l->nchars += 1;
        l->when = now;
        return;
    }
    a = g_new0(UndoAct, 1);
    a->insert = 1;
    a->pos = gtk_text_iter_get_offset(loc);
    a->text = g_strndup(text, len);
    a->nchars = (int)nchars;
    a->when = now;
    g_ptr_array_add(g_undo, a);
}

static void on_delete_range(GtkTextBuffer *b, GtkTextIter *s, GtkTextIter *e, gpointer data)
{
    UndoAct *a, *l;
    gchar *text;
    int pos = gtk_text_iter_get_offset(s), n = gtk_text_iter_get_offset(e) - pos;
    gint64 now = g_get_monotonic_time();
    (void)data;
    if (g_undoBusy || n <= 0) return;
    g_ptr_array_set_size(g_redo, 0);
    text = gtk_text_buffer_get_text(b, s, e, TRUE);
    l = last_act(g_undo);
    if (l && !l->insert && n == 1 && now - l->when < 2000000 && strcmp(text, "\n") != 0) {
        if (pos + 1 == l->pos) {                 /* Backspace again */
            gchar *t = g_strconcat(text, l->text, NULL);
            g_free(l->text); l->text = t; l->pos = pos; l->nchars++; l->when = now;
            g_free(text);
            return;
        }
        if (pos == l->pos) {                     /* Delete again */
            gchar *t = g_strconcat(l->text, text, NULL);
            g_free(l->text); l->text = t; l->nchars++; l->when = now;
            g_free(text);
            return;
        }
    }
    a = g_new0(UndoAct, 1);
    a->insert = 0;
    a->pos = pos;
    a->text = text;
    a->nchars = n;
    a->when = now;
    g_ptr_array_add(g_undo, a);
}

/* undo (from = undo list, to = redo list) or redo (the other way round) */
static void undo_step(GPtrArray *from, GPtrArray *to)
{
    UndoAct *a = last_act(from);
    GtkTextIter s, e;
    if (!a) { gtk_widget_error_bell(w_main); return; }
    g_ptr_array_index(from, from->len - 1) = NULL;       /* take it out without freeing it */
    g_ptr_array_set_size(from, from->len - 1);
    g_undoBusy = 1;
    gtk_text_buffer_get_iter_at_offset(b_edit, &s, a->pos);
    if (a->insert == (from == g_undo)) {                 /* remove the text */
        gtk_text_buffer_get_iter_at_offset(b_edit, &e, a->pos + a->nchars);
        gtk_text_buffer_delete(b_edit, &s, &e);
    } else {                                             /* put the text back */
        gtk_text_buffer_insert(b_edit, &s, a->text, -1);
    }
    gtk_text_buffer_get_iter_at_offset(b_edit, &s, a->pos +
                                       (a->insert == (from == g_undo) ? 0 : a->nchars));
    gtk_text_buffer_place_cursor(b_edit, &s);
    g_undoBusy = 0;
    a->when = 0;                                         /* never merge with a new action */
    g_ptr_array_add(to, a);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(w_edit), gtk_text_buffer_get_insert(b_edit), 0.1, FALSE, 0, 0);
}

static void update_pos(void)
{
    GtkTextIter it;
    char buf[64];
    gtk_text_buffer_get_iter_at_mark(b_edit, &it, gtk_text_buffer_get_insert(b_edit));
    snprintf(buf, sizeof buf, "Ln %d, Col %d", gtk_text_iter_get_line(&it) + 1,
             gtk_text_iter_get_line_offset(&it) + 1);
    gtk_label_set_text(GTK_LABEL(w_sbPos), buf);
}

static void on_mark_set(GtkTextBuffer *b, GtkTextIter *it, GtkTextMark *m, gpointer data)
{
    (void)it; (void)data;
    if (m == gtk_text_buffer_get_insert(b)) update_pos();
}

static gboolean clear_sim_mark(gpointer data)
{
    GtkTextIter a, b;
    (void)data;
    g_simMarkIdle = 0;
    gtk_text_buffer_get_bounds(b_edit, &a, &b);
    gtk_text_buffer_remove_tag(b_edit, t_simLine, &a, &b);
    g_simMarked = 0;
    return FALSE;
}

static void on_edit_changed(GtkTextBuffer *b, gpointer data)
{
    (void)b; (void)data;
    update_pos();
    if (g_simMarked && !g_simMarkIdle) g_simMarkIdle = g_idle_add(clear_sim_mark, NULL);
}
static void on_modified_changed(GtkTextBuffer *b, gpointer data) { (void)b; (void)data; update_title(); }

/* the kit command box shows capital letters, like the kit's own commands */
static void on_cmd_insert(GtkEditable *e, const gchar *text, gint len, gint *pos, gpointer data)
{
    gchar *up = g_utf8_strup(text, len);
    g_signal_handlers_block_by_func(e, (gpointer)on_cmd_insert, data);
    gtk_editable_insert_text(e, up, -1, pos);
    g_signal_handlers_unblock_by_func(e, (gpointer)on_cmd_insert, data);
    g_signal_stop_emission_by_name(e, "insert-text");
    g_free(up);
}

/* a file dropped on the editor: .ABS is sent to the kit, anything else is opened */
static void on_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel,
                    guint info, guint time_, gpointer data)
{
    gchar **uris;
    (void)x; (void)y; (void)data;
    if (info != URI_TARGET) return;                      /* text: the editor's own handling */
    g_signal_stop_emission_by_name(w, "drag-data-received");
    uris = gtk_selection_data_get_uris(sel);
    if (uris && uris[0]) {
        gchar *file = g_filename_from_uri(uris[0], NULL, NULL);
        if (file) {
            const char *ext = strrchr(file, '.');
            if (ext && g_ascii_strcasecmp(ext, ".abs") == 0) send_abs_file(file);
            else if (ask_save_changes()) open_path(file);
            g_free(file);
        }
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, TRUE, FALSE, time_);
}

/* ------------------------------------------------------------------ menu actions */
static void act_new(void)     { if (ask_save_changes()) set_editor_text("", NULL, NULL, NULL); }
static void act_open(void)    { do_open(); }
static void act_save(void)    { do_save(); }
static void act_saveas(void)  { save_as(); }
static void act_exit(void)    { gtk_window_close(GTK_WINDOW(w_main)); }
static void act_undo(void)    { undo_step(g_undo, g_redo); }
static void act_redo(void)    { undo_step(g_redo, g_undo); }
static void edit_signal(const char *sig)
{
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(w_main));
    if (f && (GTK_IS_TEXT_VIEW(f) || GTK_IS_ENTRY(f))) g_signal_emit_by_name(f, sig);
}
static void act_cut(void)     { edit_signal("cut-clipboard"); }
static void act_copy(void)    { edit_signal("copy-clipboard"); }
static void act_paste(void)   { edit_signal("paste-clipboard"); }
static void act_selall(void)
{
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(w_main));
    if (f && GTK_IS_ENTRY(f)) gtk_editable_select_region(GTK_EDITABLE(f), 0, -1);
    else if (f && GTK_IS_TEXT_VIEW(f)) g_signal_emit_by_name(f, "select-all", TRUE);
}
static void act_asm(void)     { do_assemble(); }
static void act_run(void)     { do_run(); }
static void act_sendabs(void) { do_send_abs(); }
static void act_stop(void)    { do_stop(); }
static void act_find(void)    { start_job(JOB_FIND, NULL, 0); }
static void act_clear(void)   { gtk_text_buffer_set_text(b_out, "", -1); g_outAtLineStart = 1; }
static void act_help(void)    { show_text_box("How to use - " APP_NAME, KITUI_HELP); }
static void act_conn(void)    { show_text_box("Connecting the kit", KITUI_CONN); }
static void act_about(void)   { show_text_box("About", KITUI_ABOUT); }
static void act_kitlog(void)
{
    gchar *uri;
    GError *err = NULL;
    if (!g_file_test(g_logPath, G_FILE_TEST_EXISTS)) { outf(KO_INFO, "No kit log yet."); return; }
    outf(KO_INFO, "Kit log: %s", g_logPath);
    uri = g_filename_to_uri(g_logPath, NULL, NULL);
    if (!uri || !g_app_info_launch_default_for_uri(uri, NULL, &err))
        outf(KO_WARN, "Open it with a text editor: %s", g_logPath);
    if (err) g_error_free(err);
    g_free(uri);
}

static void on_action(GtkMenuItem *it, gpointer data) { (void)it; ((void (*)(void))data)(); }
static void on_button(GtkButton *b, gpointer data) { (void)b; ((void (*)(void))data)(); }

static GtkWidget *add_item(GtkWidget *menu, const char *label, void (*fn)(void), guint key, GdkModifierType mods)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    g_signal_connect(it, "activate", G_CALLBACK(on_action), (gpointer)fn);
    if (key) gtk_widget_add_accelerator(it, "activate", g_accel, key, mods, GTK_ACCEL_VISIBLE);
    return it;
}

static GtkWidget *add_menu(GtkWidget *bar, const char *label)
{
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label), *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    gtk_menu_set_accel_group(GTK_MENU(menu), g_accel);
    return menu;
}

static void add_sep(GtkWidget *menu) { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); }

static GtkWidget *make_menu(void)
{
    GtkWidget *bar = gtk_menu_bar_new(), *m, *redo;
    int i;
    m = add_menu(bar, "_File");
    add_item(m, "_New", act_new, GDK_KEY_n, GDK_CONTROL_MASK);
    add_item(m, "_Open...", act_open, GDK_KEY_o, GDK_CONTROL_MASK);
    add_item(m, "_Save", act_save, GDK_KEY_s, GDK_CONTROL_MASK);
    add_item(m, "Save _As...", act_saveas, 0, 0);
    add_sep(m);
    add_item(m, "E_xit", act_exit, GDK_KEY_q, GDK_CONTROL_MASK);
    m = add_menu(bar, "_Edit");
    add_item(m, "_Undo", act_undo, GDK_KEY_z, GDK_CONTROL_MASK);
    redo = add_item(m, "_Redo", act_redo, GDK_KEY_y, GDK_CONTROL_MASK);
    gtk_widget_add_accelerator(redo, "activate", g_accel, GDK_KEY_z,     /* Ctrl+Shift+Z too */
                               GDK_CONTROL_MASK | GDK_SHIFT_MASK, 0);
    add_sep(m);
    add_item(m, "Cu_t", act_cut, GDK_KEY_x, GDK_CONTROL_MASK);
    add_item(m, "_Copy", act_copy, GDK_KEY_c, GDK_CONTROL_MASK);
    add_item(m, "_Paste", act_paste, GDK_KEY_v, GDK_CONTROL_MASK);
    add_item(m, "Select _All", act_selall, GDK_KEY_a, GDK_CONTROL_MASK);
    m = add_menu(bar, "_Kit");
    add_item(m, "_Assemble", act_asm, GDK_KEY_F7, 0);
    add_item(m, "_Run on Kit", act_run, GDK_KEY_F5, 0);
    add_item(m, "Send ._ABS file...", act_sendabs, 0, 0);
    add_item(m, "_Stop", act_stop, GDK_KEY_Escape, 0);
    add_sep(m);
    add_item(m, "_Find kit (connect)", act_find, 0, 0);
    add_item(m, "_Clear output", act_clear, 0, 0);
    m = add_menu(bar, "_Simulator");
    add_item(m, "_Run in simulator", act_simrun, GDK_KEY_F6, 0);
    add_item(m, "_Step (one instruction)", act_simstep, GDK_KEY_F8, 0);
    add_item(m, "_Pause", act_simpause, GDK_KEY_F9, 0);
    add_item(m, "R_eset", act_simreset, 0, 0);
    add_sep(m);
    w_simCheck = gtk_check_menu_item_new_with_mnemonic("Show simulator _panel");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w_simCheck), TRUE);
    g_signal_connect(w_simCheck, "toggled", G_CALLBACK(on_sim_panel), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), w_simCheck);
    w_floatCheck = gtk_check_menu_item_new_with_mnemonic("Separate _window");
    g_signal_connect(w_floatCheck, "toggled", G_CALLBACK(on_sim_float), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), w_floatCheck);
    m = add_menu(bar, "E_xamples");                  /* built into the program: no folder needed */
    for (i = 0; i < kit_nexamples; i++) {
        GtkWidget *it = gtk_menu_item_new_with_label(kit_examples[i].menu);
        g_signal_connect(it, "activate", G_CALLBACK(on_example), GINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
    }
    m = add_menu(bar, "_Templates");
    for (i = 0; i < kit_ntemplates; i++) {
        GtkWidget *it = gtk_menu_item_new_with_label(kit_templates[i].menu);
        g_signal_connect(it, "activate", G_CALLBACK(on_template), GINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
    }
    m = add_menu(bar, "_Help");
    add_item(m, "_How to use", act_help, GDK_KEY_F1, 0);
    add_item(m, "_Connecting the kit", act_conn, 0, 0);
    add_item(m, "Kit _log", act_kitlog, 0, 0);
    add_sep(m);
    add_item(m, "_About", act_about, 0, 0);
    return bar;
}

/* ------------------------------------------------------------------ window */
static GtkWidget *make_button(const char *label, void (*fn)(void))
{
    GtkWidget *b = gtk_button_new_with_label(label);
    g_signal_connect(b, "clicked", G_CALLBACK(on_button), (gpointer)fn);
    return b;
}

/* white text areas with dark text, as on Windows (the colours of the messages need it) */
static void style_text_view(GtkWidget *v, PangoFontDescription *font)
{
    static GtkCssProvider *css;
    if (!css) {
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, gtk_check_version(3, 20, 0) == NULL
            ? "textview text { background-color: #FFFFFF; color: #202020; }"
            : "GtkTextView { background-color: #FFFFFF; color: #202020; }", -1, NULL);
    }
    gtk_style_context_add_provider(gtk_widget_get_style_context(v), GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_override_font(v, font);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(v), 4);
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(s), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

static void set_tabs_8(GtkWidget *view)          /* tab = 8 characters, like the lab samples */
{
    PangoLayout *l = gtk_widget_create_pango_layout(view, "        ");
    PangoTabArray *tabs;
    int w = 0;
    pango_layout_get_pixel_size(l, &w, NULL);
    g_object_unref(l);
    if (w <= 0) return;
    tabs = pango_tab_array_new_with_positions(1, TRUE, PANGO_TAB_LEFT, w);
    gtk_text_view_set_tabs(GTK_TEXT_VIEW(view), tabs);
    pango_tab_array_free(tabs);
}

/* the editor takes dropped files (text/uri-list first, then its own text targets) */
static void setup_drop(GtkWidget *view)
{
    GtkTargetList *old = gtk_drag_dest_get_target_list(view), *list = gtk_target_list_new(NULL, 0);
    gtk_target_list_add_uri_targets(list, URI_TARGET);
    if (old) {
        gint n = 0;
        GtkTargetEntry *t = gtk_target_table_new_from_list(old, &n);
        gtk_target_list_add_table(list, t, n);
        gtk_target_table_free(t, n);
    }
    gtk_drag_dest_set_target_list(view, list);
    gtk_target_list_unref(list);
    g_signal_connect(view, "drag-data-received", G_CALLBACK(on_drop), NULL);
}

static void stop_worker(void)
{
    if (!g_thread) return;
    __atomic_store_n(&g_cancel, 1, __ATOMIC_SEQ_CST);
    g_mutex_lock(&g_mx);
    g_quit = 1;
    g_cond_signal(&g_cv);
    g_mutex_unlock(&g_mx);
    g_thread_join(g_thread);
    g_thread = NULL;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w; (void)ev; (void)data;
    if (!ask_save_changes()) return TRUE;               /* stay open */
    stop_worker();
    g_closing = 1;
    return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer data) { (void)w; (void)data; gtk_main_quit(); }

static void set_icon(void)
{
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type("png", NULL);
    if (!ld) return;
    if (gdk_pixbuf_loader_write(ld, icon_png, sizeof icon_png, NULL) && gdk_pixbuf_loader_close(ld, NULL)) {
        GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(ld);
        if (pb) gtk_window_set_default_icon(pb);
    }
    g_object_unref(ld);
}

static GtkWidget *sim_button(const char *label, void (*fn)(void), const char *tip)
{
    GtkWidget *b = make_button(label, fn);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

/* right side: simulator buttons, speed and the kit picture */
static GtkWidget *build_sim_panel(void)
{
    GtkWidget *box, *row, *srow, *lbl, *frame;
    int i;
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(row), 6);
    gtk_box_pack_start(GTK_BOX(row), sim_button("Run (F6)", act_simrun, "Run the program in the simulator (no kit needed)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), sim_button("Step (F8)", act_simstep, "Run one instruction; the next line is marked yellow"), FALSE, FALSE, 0);
    w_simPause = sim_button("Pause (F9)", act_simpause, "Stop here (Run continues, Step goes on one by one)");
    gtk_box_pack_start(GTK_BOX(row), w_simPause, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), sim_button("Reset", act_simreset, "Start the program again from 0000:1000"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    srow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_left(srow, 8);
    gtk_widget_set_margin_right(srow, 8);
    lbl = gtk_label_new("Speed:");
    gtk_box_pack_start(GTK_BOX(srow), lbl, FALSE, FALSE, 0);
    w_speed = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 6, 1);
    gtk_scale_set_draw_value(GTK_SCALE(w_speed), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(w_speed), 0);
    for (i = 0; i <= 6; i++) gtk_scale_add_mark(GTK_SCALE(w_speed), i, GTK_POS_BOTTOM, NULL);
    gtk_range_set_value(GTK_RANGE(w_speed), 6);
    gtk_widget_set_size_request(w_speed, 110, -1);
    gtk_widget_set_tooltip_text(w_speed, "Slow motion: move left to watch the displays change step by step");
    g_signal_connect(w_speed, "value-changed", G_CALLBACK(on_speed), NULL);
    gtk_box_pack_start(GTK_BOX(srow), w_speed, TRUE, TRUE, 0);
    w_speedLbl = gtk_label_new("");
    gtk_label_set_width_chars(GTK_LABEL(w_speedLbl), 20);
    gtk_misc_set_alignment(GTK_MISC(w_speedLbl), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(srow), w_speedLbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), srow, FALSE, FALSE, 0);

    w_simArea = gtk_drawing_area_new();
    gtk_widget_set_size_request(w_simArea, 300, 330);
    g_signal_connect(w_simArea, "draw", G_CALLBACK(on_sim_draw), NULL);
    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(frame), w_simArea);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 6);
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);
    on_speed(GTK_RANGE(w_speed), NULL);
    return box;
}

static void build_window(void)
{
    GtkWidget *vbox, *left, *tool, *paned, *cmdrow, *status, *lbl;
    PangoFontDescription *mono = pango_font_description_from_string("Monospace 10");
    GtkTextIter end;
    GdkRectangle wa;
    GdkScreen *scr = gdk_screen_get_default();
    int i, W = 1320, H = 820, simW;

    /* window size: room for the editor and the simulator, but never bigger than the screen */
    gdk_screen_get_monitor_workarea(scr, gdk_screen_get_primary_monitor(scr), &wa);
    if (wa.width > 200 && W > wa.width - 16) W = wa.width - 16;
    if (wa.height > 200 && H > wa.height - 40) H = wa.height - 40;
    simW = W * 45 / 100;
    if (W - simW < 620) simW = W - 620;
    if (simW < 330) simW = 330;

    w_main = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(w_main), W, H);
    gtk_window_set_position(GTK_WINDOW(w_main), GTK_WIN_POS_CENTER);
    if (wa.width > 200 && wa.width < 1200) gtk_window_maximize(GTK_WINDOW(w_main));
    g_accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(w_main), g_accel);
    g_signal_connect(w_main, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(w_main, "destroy", G_CALLBACK(on_destroy), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(w_main), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), make_menu(), FALSE, FALSE, 0);

    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    tool = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tool), 6);
    w_btnAsm = make_button("Assemble (F7)", act_asm);
    w_btnRun = make_button("Run on Kit (F5)", act_run);
    w_btnStop = make_button("Stop", act_stop);
    gtk_box_pack_start(GTK_BOX(tool), w_btnAsm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tool), w_btnRun, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tool), w_btnStop, FALSE, FALSE, 0);
    lbl = gtk_label_new("Port:");
    gtk_box_pack_start(GTK_BOX(tool), lbl, FALSE, FALSE, 6);
    w_port = gtk_combo_box_text_new();
    gtk_widget_set_size_request(w_port, 150, -1);
    g_signal_connect(w_port, "changed", G_CALLBACK(on_port_changed), NULL);
    gtk_box_pack_start(GTK_BOX(tool), w_port, FALSE, FALSE, 0);
    w_btnRefresh = make_button("Refresh", fill_ports);
    gtk_box_pack_start(GTK_BOX(tool), w_btnRefresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), tool, FALSE, FALSE, 0);

    w_edit = gtk_text_view_new();
    b_edit = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w_edit));
    style_text_view(w_edit, mono);
    set_tabs_8(w_edit);
    t_simLine = gtk_text_buffer_create_tag(b_edit, NULL, "paragraph-background", "#FFE98A", NULL);
    g_signal_connect(b_edit, "insert-text", G_CALLBACK(on_insert_text), NULL);
    g_signal_connect(b_edit, "delete-range", G_CALLBACK(on_delete_range), NULL);
    g_signal_connect(b_edit, "mark-set", G_CALLBACK(on_mark_set), NULL);
    g_signal_connect(b_edit, "changed", G_CALLBACK(on_edit_changed), NULL);
    g_signal_connect(b_edit, "modified-changed", G_CALLBACK(on_modified_changed), NULL);
    setup_drop(w_edit);

    w_out = gtk_text_view_new();
    b_out = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w_out));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(w_out), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(w_out), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w_out), GTK_WRAP_WORD_CHAR);
    style_text_view(w_out, mono);
    for (i = 0; i < 12; i++)
        t_kind[i] = gtk_text_buffer_create_tag(b_out, NULL, "foreground", kind_color(i), NULL);
    gtk_text_buffer_get_end_iter(b_out, &end);
    m_outEnd = gtk_text_buffer_create_mark(b_out, "end", &end, FALSE);
    g_signal_connect(w_out, "button-press-event", G_CALLBACK(on_out_button), NULL);

    paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(paned), scrolled(w_edit), TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), scrolled(w_out), TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), H > 600 ? H - 330 : 300);
    gtk_container_set_border_width(GTK_CONTAINER(paned), 6);
    gtk_box_pack_start(GTK_BOX(left), paned, TRUE, TRUE, 0);

    cmdrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(cmdrow), 6);
    w_cmd = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(w_cmd), 120);
    gtk_entry_set_width_chars(GTK_ENTRY(w_cmd), 10);
    gtk_widget_override_font(w_cmd, mono);
    g_signal_connect(w_cmd, "insert-text", G_CALLBACK(on_cmd_insert), NULL);
    g_signal_connect(w_cmd, "activate", G_CALLBACK(on_button), (gpointer)do_send_cmd);
    w_btnSend = make_button("Send to kit", do_send_cmd);
    gtk_box_pack_start(GTK_BOX(cmdrow), w_cmd, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cmdrow), w_btnSend, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), cmdrow, FALSE, FALSE, 0);

    w_simBox = build_sim_panel();
    w_hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(w_hpaned), left, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(w_hpaned), w_simBox, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(w_hpaned), W - simW);
    gtk_box_pack_start(GTK_BOX(vbox), w_hpaned, TRUE, TRUE, 0);

    status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(status), 4);
    w_sbPos = gtk_label_new("Ln 1, Col 1");
    gtk_widget_set_size_request(w_sbPos, 110, -1);
    gtk_widget_set_halign(w_sbPos, GTK_ALIGN_START);
    w_sbFile = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(w_sbFile), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(w_sbFile, GTK_ALIGN_START);
    w_sbKit = gtk_label_new("Kit: not connected");
    gtk_box_pack_start(GTK_BOX(status), w_sbPos, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status), w_sbFile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(status), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status), w_sbKit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status, FALSE, FALSE, 0);

    pango_font_description_free(mono);
}

/* private folder for the assembler's files: /tmp/kitstudio-<user id>/ */
static void make_folders(void)
{
    struct stat st;
    char *cache;
    snprintf(g_tmpdir, sizeof g_tmpdir, "%s/kitstudio-%u/", g_get_tmp_dir(), (unsigned)getuid());
    mkdir(g_tmpdir, 0700);
    if (lstat(g_tmpdir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || access(g_tmpdir, W_OK) != 0) {
        snprintf(g_tmpdir, sizeof g_tmpdir, "%s/kitstudio/tmp/", g_get_user_cache_dir());
        g_mkdir_with_parents(g_tmpdir, 0700);
    }
    cache = g_build_filename(g_get_user_cache_dir(), "kitstudio", NULL);
    g_mkdir_with_parents(cache, 0700);
    snprintf(g_logPath, sizeof g_logPath, "%s/kit_log.txt", cache);
    g_free(cache);
}

int main(int argc, char **argv)
{
    g_set_prgname("kitstudio");
    gtk_init(&argc, &argv);
    g_set_application_name(APP_NAME);
    g_mutex_init(&g_mx);
    g_cond_init(&g_cv);
    g_mutex_init(&g_qmx);
    g_undo = g_ptr_array_new_with_free_func(undo_free_act);
    g_redo = g_ptr_array_new_with_free_func(undo_free_act);
    make_folders();
    set_icon();
    kitsim_unload(&g_sim);          /* empty simulator: nothing loaded yet */
    g_sim.panel.speed = 1.0;
    build_window();

    set_editor_text("", NULL, NULL, NULL);       /* always start with an empty editor */
    outf(KO_INFO, APP_NAME " " APP_VERSION " - write a program, press F7 to assemble, F5 to run it on the kit.");
    outf(KO_INFO, "Kit: SELECT MODE switch = PC. If it does not answer, press RESET on the kit. Help: F1.");
    fill_ports();
    set_busy_ui(0, NULL);
    gtk_widget_show_all(w_main);
    {
        GdkRectangle wa;
        GdkScreen *scr = gdk_screen_get_default();
        gdk_screen_get_monitor_workarea(scr, gdk_screen_get_primary_monitor(scr), &wa);
        if (wa.width > 200 && wa.width < 1200)       /* not enough room side by side */
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w_floatCheck), TRUE);
    }
    sim_show();
    gtk_widget_grab_focus(w_edit);
    if (argc > 1) open_path(argv[1]);

    g_thread = g_thread_new("kit", worker, NULL);
    gtk_main();
    stop_worker();
    sim_stop_timer();
    kitsim_unload(&g_sim);
    return 0;
}
