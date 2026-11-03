/* MDA-8086 Kit Studio - Windows window
 * Write, assemble and run programs on the Midas MDA-8086 trainer kit.
 * One 32-bit Windows program: runs on Windows XP, 7, 8, 10 and 11 (32 or 64 bit).
 * (The Linux window is main_gtk.c; both use kitui.c and kitbuiltin.c.)
 *
 * - built-in assembler (JWasm, MASM syntax, output identical to the lab's TASM+LINK+MEXE2ABS)
 * - finds the kit on any COM port (built-in or USB-to-RS232 adapter)
 * - sends the program with the kit's L command and starts it with G 0000:1000
 * - terminal line for the kit's monitor commands (D, E, R, ...)
 * - built-in example programs and templates
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "kitasm.h"
#include "kitcore.h"
#include "kitsim.h"
#include "kithint.h"
#include "kitui.h"
#include "kitbuiltin.h"
#include "resource.h"

#define WM_KITTEXT  (WM_APP + 1)     /* wParam = kind, lParam = malloc'd text */
#define WM_KITSTATE (WM_APP + 2)     /* wParam = busy, lParam = malloc'd port name or NULL */

enum {
    IDC_EDIT = 100, IDC_OUT, IDC_CMD, IDC_STATUS, IDC_PORT,
    IDC_ASM, IDC_RUN, IDC_STOP, IDC_REFRESH, IDC_SEND, IDC_PORTLBL,
    IDM_NEW = 200, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_EXIT,
    IDM_UNDO, IDM_CUT, IDM_COPY, IDM_PASTE, IDM_SELALL,
    IDM_ASM, IDM_RUN, IDM_SENDABS, IDM_STOP, IDM_FIND, IDM_CLEAR,
    IDM_HELP, IDM_HELP_CONN, IDM_KITLOG, IDM_ABOUT,
    IDM_EX_FIRST = 300,             /* 300.. : built-in examples (kit_examples[]) */
    IDM_TPL_FIRST = 340             /* 340.. : built-in templates (kit_templates[]) */
};

enum { JOB_NONE, JOB_RUN, JOB_CMD, JOB_FIND, JOB_PORT };

/* ------------------------------------------------------------------ globals */
static HINSTANCE g_inst;
static HWND g_main, g_edit, g_out, g_cmd, g_status, g_port, g_btnAsm, g_btnRun, g_btnStop,
            g_btnRefresh, g_btnSend, g_portLbl;
static HFONT g_fontMono, g_fontUI;
static HACCEL g_accel;
static int g_dpi = 96;
static char g_path[MAX_PATH];          /* current file, "" = not saved yet */
static char g_shownName[MAX_PATH];     /* name shown for an unsaved copy of an example */
static char g_origin[64];              /* "Template 1": a new program made from a template */
static char g_tmpdir[MAX_PATH];
static char g_logPath[MAX_PATH + 16];  /* "<temp>\KitStudio\kit_log.txt": every byte to/from the kit */
static int g_outAtLineStart = 1;
static int g_lastLine = -1, g_lastCol = -1;
static char g_sbFile[MAX_PATH + 96], g_sbKit[128];

/* worker thread */
static CRITICAL_SECTION g_cs;
static HANDLE g_wake, g_thread;
static volatile long g_cancel, g_quit, g_busy;
static int g_job;
static char *g_jobText;                /* ABS text or command line */
static int g_jobGo;
static char g_jobPort[KP_NAMELEN];
static KitLink g_kit;                  /* used only by the worker thread */

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ------------------------------------------------------------------ output pane */
static COLORREF kind_color(int kind)
{
    switch (kind) {
    case KO_OK:   return RGB(0, 128, 0);
    case KO_WARN: return RGB(185, 100, 0);
    case KO_ERR:  return RGB(205, 0, 0);
    case KO_KIT:  return RGB(0, 70, 165);
    case KO_DUMP: return RGB(110, 110, 110);   /* machine code dump */
    case KO_HINT: return RGB(120, 50, 160);    /* hints for assembler errors */
    case KO_CHECK: return RGB(0, 105, 120);    /* kit check (model of the kit) */
    default:      return RGB(40, 40, 40);
    }
}

static void out_raw(COLORREF color, const char *text)
{
    CHARRANGE cr;
    CHARFORMAT2A cf;
    char *buf, *q;
    const char *p;
    int len = GetWindowTextLengthA(g_out);
    if (len > 400000) {                 /* keep the pane small */
        cr.cpMin = 0; cr.cpMax = 200000;
        SendMessageA(g_out, EM_EXSETSEL, 0, (LPARAM)&cr);
        SendMessageA(g_out, EM_REPLACESEL, FALSE, (LPARAM)"");
    }
    buf = (char *)malloc(strlen(text) * 2 + 1);
    if (!buf) return;
    for (p = text, q = buf; *p; p++) {  /* '\n' -> "\r\n" */
        if (*p == '\n') *q++ = '\r';
        if (*p != '\r') *q++ = *p;
    }
    *q = 0;
    cr.cpMin = cr.cpMax = -1;
    SendMessageA(g_out, EM_EXSETSEL, 0, (LPARAM)&cr);
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = color;
    SendMessageA(g_out, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageA(g_out, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageA(g_out, WM_VSCROLL, SB_BOTTOM, 0);
    if (q > buf) g_outAtLineStart = (q[-1] == '\n');
    free(buf);
}

/* messages always start on a new line; kit text is shown exactly as it comes */
static void out_msg(int kind, const char *text)
{
    if (kind != KO_KIT) {
        while (*text == '\n') text++;
        if (!g_outAtLineStart) out_raw(kind_color(KO_INFO), "\n");
        out_raw(kind_color(kind), text);
        if (!g_outAtLineStart) out_raw(kind_color(kind), "\n");
    } else {
        out_raw(kind_color(kind), text);
    }
}

static void outf(int kind, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    buf[sizeof buf - 1] = 0;
    va_end(ap);
    out_msg(kind, buf);
}

/* called from the worker thread */
static void worker_out(void *ctx, int kind, const char *text)
{
    char *copy = _strdup(text);
    (void)ctx;
    if (copy && !PostMessageA(g_main, WM_KITTEXT, (WPARAM)kind, (LPARAM)copy)) free(copy);
}

static void post_state(void)
{
    char *name = g_kit.port ? _strdup(kp_name(g_kit.port)) : NULL;
    if (!PostMessageA(g_main, WM_KITSTATE, (WPARAM)g_busy, (LPARAM)name)) free(name);
}

/* ------------------------------------------------------------------ worker thread */
/* kit log: appended each session, started again when it is over 1 MB */
static FILE *open_kit_log(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    SYSTEMTIME st;
    FILE *f;
    int big = GetFileAttributesExA(g_logPath, GetFileExInfoStandard, &fa) &&
              (fa.nFileSizeHigh || fa.nFileSizeLow > 1000000);
    f = fopen(g_logPath, big ? "w" : "a");
    if (!f) return NULL;
    GetLocalTime(&st);
    fprintf(f, "\n===== " APP_NAME " " APP_VERSION "   %04d-%02d-%02d %02d:%02d:%02d =====\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(f);
    return f;
}

static unsigned __stdcall worker(void *arg)
{
    FILE *log;
    (void)arg;
    kit_init(&g_kit, worker_out, NULL, &g_cancel);
    log = open_kit_log();
    if (log) kit_set_log(&g_kit, log);
    for (;;) {
        int job;
        char *text;
        int go;
        WaitForSingleObject(g_wake, g_kit.port ? 30 : 300);
        if (g_quit) break;
        EnterCriticalSection(&g_cs);
        job = g_job; text = g_jobText; go = g_jobGo;
        if (job == JOB_PORT) {
            snprintf(g_kit.want, sizeof g_kit.want, "%s", g_jobPort);
        }
        g_job = JOB_NONE; g_jobText = NULL;
        LeaveCriticalSection(&g_cs);

        if (job == JOB_NONE) {
            if (g_kit.port && kit_poll(&g_kit) < 0) post_state();
            continue;
        }
        InterlockedExchange(&g_busy, 1);
        InterlockedExchange(&g_cancel, 0);
        post_state();
        switch (job) {
        case JOB_PORT:
            kit_disconnect(&g_kit);
            break;
        case JOB_FIND:
            kit_find(&g_kit, 1);
            break;
        case JOB_CMD:          /* make sure the monitor is at its prompt first */
            if (kit_find(&g_kit, 1))
                kit_command(&g_kit, text);
            break;
        case JOB_RUN: {
            int n = 0;
            if (kit_find(&g_kit, 1) && kit_download(&g_kit, text, &n) && go)
                kit_go(&g_kit, 0, 0x1000);
            break;
        }
        }
        free(text);
        InterlockedExchange(&g_busy, 0);
        post_state();
    }
    kit_disconnect(&g_kit);
    g_kit.log = NULL;
    if (log) fclose(log);
    return 0;
}

static int start_job(int job, const char *text, int go)
{
    int ok = 0;
    EnterCriticalSection(&g_cs);
    if (!g_busy && g_job == JOB_NONE) {
        g_job = job;
        g_jobText = text ? _strdup(text) : NULL;
        g_jobGo = go;
        ok = 1;
    }
    LeaveCriticalSection(&g_cs);
    if (ok) SetEvent(g_wake);
    else MessageBeep(MB_ICONEXCLAMATION);
    return ok;
}

/* ------------------------------------------------------------------ files */
static char *read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf, *out, *q;
    long i;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (char *)malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    n = (long)fread(buf, 1, n, f);
    fclose(f);
    buf[n] = 0;
    out = (char *)malloc(n * 2 + 1);   /* make every line end with CR LF for the editor */
    if (!out) { free(buf); return NULL; }
    for (i = 0, q = out; i < n; i++) {
        char c = buf[i];
        if (c == 0) c = ' ';
        if (c == '\r') {
            *q++ = '\r'; *q++ = '\n';
            if (i + 1 < n && buf[i + 1] == '\n') i++;
        } else if (c == '\n') {
            *q++ = '\r'; *q++ = '\n';
        } else {
            *q++ = c;
        }
    }
    *q = 0;
    free(buf);
    return out;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    size_t n = strlen(text);
    if (!f) return 0;
    if (fwrite(text, 1, n, f) != n) { fclose(f); return 0; }
    return fclose(f) == 0;
}

static char *editor_text(void)
{
    int n = GetWindowTextLengthA(g_edit);
    char *buf = (char *)malloc(n + 1);
    if (!buf) return NULL;
    GetWindowTextA(g_edit, buf, n + 1);
    return buf;
}

static const char *file_part(const char *path) { return kitui_file_part(path); }

/* name used in the title bar and in assembler messages */
static const char *shown_name(void)
{
    if (g_path[0]) return file_part(g_path);
    if (g_shownName[0]) return g_shownName;
    return "Untitled";
}

static void update_title(void)
{
    char t[MAX_PATH + 64];
    int mod = (int)SendMessageA(g_edit, EM_GETMODIFY, 0, 0);
    snprintf(t, sizeof t, "%s%s - %s", shown_name(), mod ? "*" : "", APP_NAME);
    SetWindowTextA(g_main, t);
    if (g_path[0])
        snprintf(g_sbFile, sizeof g_sbFile, "%s%s", g_path, mod ? "  (changed)" : "");
    else if (g_shownName[0])
        snprintf(g_sbFile, sizeof g_sbFile, "%s (copy of an example, not saved yet)%s", g_shownName,
                 mod ? "  (changed)" : "");
    else if (g_origin[0])
        snprintf(g_sbFile, sizeof g_sbFile, "Untitled (new program from %s, not saved yet)%s", g_origin,
                 mod ? "  (changed)" : "");
    else
        snprintf(g_sbFile, sizeof g_sbFile, "Untitled (not saved yet)%s", mod ? "  (changed)" : "");
    SendMessageA(g_status, SB_SETTEXTA, 1, (LPARAM)g_sbFile);
}

/* path = file on disk (or NULL); name = name to show for an unsaved copy of an example (or
   NULL); origin = "Template 1" for a new program made from a template (or NULL) */
static void set_editor_text(const char *text, const char *path, const char *name, const char *origin)
{
    SetWindowTextA(g_edit, text);
    SendMessageA(g_edit, EM_SETMODIFY, FALSE, 0);
    SendMessageA(g_edit, EM_EMPTYUNDOBUFFER, 0, 0);
    snprintf(g_path, sizeof g_path, "%s", path ? path : "");
    snprintf(g_shownName, sizeof g_shownName, "%s", (!path && name) ? name : "");
    snprintf(g_origin, sizeof g_origin, "%s", (!path && origin) ? origin : "");
    update_title();
}

static int save_to(const char *path)
{
    char *text = editor_text();
    int ok = text && write_text_file(path, text);
    free(text);
    if (!ok) {
        char msg[MAX_PATH + 100];
        snprintf(msg, sizeof msg, "Could not save the file:\n%s\n\nTry File > Save As in another folder (e.g. Documents).", path);
        MessageBoxA(g_main, msg, APP_NAME, MB_ICONERROR);
        return 0;
    }
    if (path != g_path) snprintf(g_path, sizeof g_path, "%s", path);   /* (never copy onto itself) */
    g_shownName[0] = 0;
    g_origin[0] = 0;
    SendMessageA(g_edit, EM_SETMODIFY, FALSE, 0);
    update_title();
    return 1;
}

static int save_as(void)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH];
    snprintf(file, sizeof file, "%s", g_path[0] ? file_part(g_path) :
             (g_shownName[0] ? g_shownName : "PROGRAM.ASM"));
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "Assembly program (*.asm)\0*.asm\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrDefExt = "asm";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameA(&ofn)) return 0;
    return save_to(file);
}

static int do_save(void)
{
    if (!g_path[0]) return save_as();
    return save_to(g_path);
}

/* returns 0 if the user cancelled */
static int ask_save_changes(void)
{
    int r;
    if (!SendMessageA(g_edit, EM_GETMODIFY, 0, 0)) return 1;
    r = MessageBoxA(g_main, "The program was changed. Save it first?", APP_NAME,
                    MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return 0;
    if (r == IDYES) return do_save();
    return 1;
}

static void open_path(const char *path)
{
    char *text = read_text_file(path);
    if (!text) {
        char msg[MAX_PATH + 64];
        snprintf(msg, sizeof msg, "Could not open:\n%s", path);
        MessageBoxA(g_main, msg, APP_NAME, MB_ICONERROR);
        return;
    }
    set_editor_text(text, path, NULL, NULL);
    free(text);
    outf(KO_INFO, "Opened %s", path);
}

static void do_open(void)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = "";
    if (!ask_save_changes()) return;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "Assembly program (*.asm)\0*.asm\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameA(&ofn)) open_path(file);
}

/* ------------------------------------------------------------------ built-in examples, templates */
/* the program text with CR LF line ends, as the edit box needs */
static char *crlf_text(const char *text)
{
    size_t n = strlen(text), i;
    char *out = (char *)malloc(n * 2 + 1), *q = out;
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        if (text[i] == '\n') *q++ = '\r';
        *q++ = text[i];
    }
    *q = 0;
    return out;
}

/* an example opens as an unsaved copy; a template as a new untitled program */
static void load_builtin(const KitBuiltin *b, int is_template)
{
    char origin[64], *dash, *text;
    if (!ask_save_changes()) return;
    text = crlf_text(b->text);
    if (!text) return;
    snprintf(origin, sizeof origin, "%s", b->menu);
    dash = strstr(origin, " - ");               /* "Template 1 - Short form ..." -> "Template 1" */
    if (dash) *dash = 0;
    set_editor_text(text, NULL, is_template ? NULL : b->name, is_template ? origin : NULL);
    free(text);
    if (is_template)
        outf(KO_INFO, "New program from %s: write your program where the comments say, "
             "then File > Save As.", origin);
    else
        outf(KO_INFO, "Opened example %s (a copy: File > Save As keeps your own version).", b->name);
}

/* ------------------------------------------------------------------ assemble */
static void ui_out(int kind, const char *text) { out_msg(kind, text); }

/* assembles the editor text; on success returns malloc'd .ABS text */
static char *assemble(int quiet_dump)
{
    char src[MAX_PATH];
    char *abs, *text;

    if (g_path[0]) {
        if (!save_to(g_path)) return NULL;
        snprintf(src, sizeof src, "%s", g_path);
    } else {
        char *text = editor_text();
        snprintf(src, sizeof src, "%sUNTITLED.ASM", g_tmpdir);
        if (!text || !write_text_file(src, text)) {
            free(text);
            outf(KO_ERR, "Cannot write a temporary file in %s", g_tmpdir);
            return NULL;
        }
        free(text);
    }
    if (!g_outAtLineStart) out_raw(kind_color(KO_INFO), "\n");
    outf(KO_INFO, "---- Assembling %s ----", g_path[0] ? file_part(g_path) :
         (g_shownName[0] ? g_shownName : "(not saved yet)"));
    text = editor_text();
    SetCursor(LoadCursor(NULL, IDC_WAIT));
    abs = kitui_assemble(src, g_path[0] ? file_part(g_path) : (g_shownName[0] ? g_shownName : "UNTITLED.ASM"),
                         text, g_tmpdir, g_path[0] ? g_path : NULL, quiet_dump, ui_out);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
    free(text);
    return abs;
}

static void do_run(void)
{
    char *abs;
    if (g_busy) { MessageBeep(MB_ICONEXCLAMATION); return; }
    abs = assemble(1);
    if (!abs) return;
    start_job(JOB_RUN, abs, 1);
    free(abs);
}

static void do_send_abs(void)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = "";
    char *text, why[200];
    int n = 0, recs;
    if (g_busy) { MessageBeep(MB_ICONEXCLAMATION); return; }
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "Kit program (*.abs)\0*.abs\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameA(&ofn)) return;
    text = read_text_file(file);
    if (!text) { outf(KO_ERR, "Could not read %s", file); return; }
    recs = kit_check_abs(text, &n, why, sizeof why);
    if (recs <= 0) {
        outf(KO_ERR, "%s: %s", file_part(file), recs < 0 ? why : "no program bytes in this file");
        free(text);
        return;
    }
    outf(KO_INFO, "---- Sending %s (%d bytes) ----", file_part(file), n);
    start_job(JOB_RUN, text, 1);
    free(text);
}

/* jump to the line number found in an assembler message like "X.ASM(12) : Error ..."
   or in a hint like "Hint (line 12): ..." */
static void goto_error_line(const char *line)
{
    int num = kitui_error_line(line);
    if (num <= 0) return;
    {
        int start = (int)SendMessageA(g_edit, EM_LINEINDEX, num - 1, 0);
        int len;
        if (start < 0) return;
        len = (int)SendMessageA(g_edit, EM_LINELENGTH, start, 0);
        SetFocus(g_edit);
        SendMessageA(g_edit, EM_SETSEL, start, start + len);
        SendMessageA(g_edit, EM_SCROLLCARET, 0, 0);
    }
}

/* ------------------------------------------------------------------ ports */
static void fill_ports(void)
{
    char names[KP_MAXPORTS][KP_NAMELEN];
    char cur[KP_NAMELEN] = "";
    int n, i, sel = 0, idx = (int)SendMessageA(g_port, CB_GETCURSEL, 0, 0);
    if (idx > 0) SendMessageA(g_port, CB_GETLBTEXT, idx, (LPARAM)cur);
    n = kp_list(names, KP_MAXPORTS);
    SendMessageA(g_port, CB_RESETCONTENT, 0, 0);
    SendMessageA(g_port, CB_ADDSTRING, 0, (LPARAM)"Auto (find kit)");
    for (i = 0; i < n; i++) {
        SendMessageA(g_port, CB_ADDSTRING, 0, (LPARAM)names[i]);
        if (cur[0] && strcmp(cur, names[i]) == 0) sel = i + 1;
    }
    SendMessageA(g_port, CB_SETCURSEL, sel, 0);
    if (n == 0) outf(KO_WARN, "No COM port found on this PC. Connect the kit with a COM port or a "
                     "USB-to-RS232 adapter, then press Refresh.");
    else {
        char list[KP_MAXPORTS * 8] = "";
        for (i = 0; i < n; i++) { if (i) strcat(list, ", "); strcat(list, names[i]); }
        outf(KO_INFO, "COM ports on this PC: %s", list);
    }
}

static void port_changed(void)
{
    char name[KP_NAMELEN] = "";
    int idx = (int)SendMessageA(g_port, CB_GETCURSEL, 0, 0);
    if (idx > 0) SendMessageA(g_port, CB_GETLBTEXT, idx, (LPARAM)name);
    EnterCriticalSection(&g_cs);
    snprintf(g_jobPort, sizeof g_jobPort, "%s", name);
    LeaveCriticalSection(&g_cs);
    if (!start_job(JOB_PORT, NULL, 0)) {
        outf(KO_WARN, "Busy - the port change will apply after the current job.");
    } else {
        outf(KO_INFO, "Port: %s", name[0] ? name : "Auto (find the kit on any COM port)");
    }
}

static void set_busy_ui(int busy, const char *port)
{
    EnableWindow(g_btnRun, !busy);
    EnableWindow(g_btnSend, !busy);
    EnableWindow(g_btnStop, busy);
    EnableWindow(g_port, !busy);
    EnableWindow(g_btnRefresh, !busy);
    if (busy) snprintf(g_sbKit, sizeof g_sbKit, "Kit: working...  (Stop to cancel)");
    else if (port) snprintf(g_sbKit, sizeof g_sbKit, "Kit: connected on %s", port);
    else snprintf(g_sbKit, sizeof g_sbKit, "Kit: not connected");
    SendMessageA(g_status, SB_SETTEXTA, 2, (LPARAM)g_sbKit);
}

/* ------------------------------------------------------------------ help */
static void show_text_box(const char *title, const char *text)
{
    MessageBoxA(g_main, text, title, MB_OK | MB_ICONINFORMATION);
}

/* ------------------------------------------------------------------ layout */
static void layout(void)
{
    RECT rc, sr;
    int m = S(6), bh = S(26), x, y, w, h, sh, outH, cmdH = S(24), top;
    GetClientRect(g_main, &rc);
    SendMessageA(g_status, WM_SIZE, 0, 0);
    GetWindowRect(g_status, &sr);
    sh = sr.bottom - sr.top;
    {   /* status bar parts */
        int parts[3];
        parts[0] = S(110);
        parts[1] = rc.right - S(250) > parts[0] ? rc.right - S(250) : parts[0] + 10;
        parts[2] = -1;
        SendMessageA(g_status, SB_SETPARTS, 3, (LPARAM)parts);
        SendMessageA(g_status, SB_SETTEXTA, 1, (LPARAM)g_sbFile);
        SendMessageA(g_status, SB_SETTEXTA, 2, (LPARAM)g_sbKit);
        g_lastLine = -1;
    }
    x = m; y = m;
    MoveWindow(g_btnAsm, x, y, S(110), bh, TRUE); x += S(110) + m;
    MoveWindow(g_btnRun, x, y, S(130), bh, TRUE); x += S(130) + m;
    MoveWindow(g_btnStop, x, y, S(60), bh, TRUE); x += S(60) + S(18);
    MoveWindow(g_portLbl, x, y + S(5), S(32), bh - S(5), TRUE); x += S(34);
    MoveWindow(g_port, x, y + S(1), S(140), S(200), TRUE); x += S(140) + m;
    MoveWindow(g_btnRefresh, x, y, S(70), bh, TRUE);
    top = y + bh + m;
    h = rc.bottom - sh - top;
    outH = h * 34 / 100;
    if (outH < S(90)) outH = S(90);
    w = rc.right - 2 * m;
    MoveWindow(g_edit, m, top, w, h - outH - cmdH - 2 * m, TRUE);
    MoveWindow(g_out, m, top + h - outH - cmdH - m, w, outH, TRUE);
    MoveWindow(g_cmd, m, top + h - cmdH - m / 2, w - S(90) - m, cmdH, TRUE);
    MoveWindow(g_btnSend, m + w - S(90), top + h - cmdH - m / 2, S(90), cmdH, TRUE);
}

static void update_line_col(void)
{
    DWORD s = 0, e = 0;
    int line, col;
    char buf[64];
    SendMessageA(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    line = (int)SendMessageA(g_edit, EM_LINEFROMCHAR, e, 0);
    col = (int)e - (int)SendMessageA(g_edit, EM_LINEINDEX, line, 0);
    if (line != g_lastLine || col != g_lastCol) {
        g_lastLine = line; g_lastCol = col;
        snprintf(buf, sizeof buf, "Ln %d, Col %d", line + 1, col + 1);
        SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)buf);
    }
}

/* Enter in the kit command box sends the command */
static WNDPROC g_cmdOldProc;
static LRESULT CALLBACK cmd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(g_main, WM_COMMAND, IDC_SEND, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    return CallWindowProcA(g_cmdOldProc, h, msg, wp, lp);
}

static int CALLBACK font_exists_cb(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM lp)
{
    (void)lf; (void)tm; (void)type;
    *(int *)lp = 1;
    return 0;
}

static HFONT make_mono_font(int pt)
{
    LOGFONTA lf;
    HDC dc = GetDC(NULL);
    int found = 0;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    strcpy(lf.lfFaceName, "Consolas");
    EnumFontFamiliesExA(dc, &lf, (FONTENUMPROCA)font_exists_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = ANSI_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lf.lfQuality = CLEARTYPE_QUALITY;
    strcpy(lf.lfFaceName, found ? "Consolas" : "Courier New");
    return CreateFontIndirectA(&lf);
}

static HWND make_button(const char *text, int id)
{
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 10, 10, g_main, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
    return h;
}

static HMENU make_menu(void)
{
    HMENU bar = CreateMenu(), m;
    int i;
    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, IDM_NEW, "&New\tCtrl+N");
    AppendMenuA(m, MF_STRING, IDM_OPEN, "&Open...\tCtrl+O");
    AppendMenuA(m, MF_STRING, IDM_SAVE, "&Save\tCtrl+S");
    AppendMenuA(m, MF_STRING, IDM_SAVEAS, "Save &As...");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)m, "&File");
    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, IDM_UNDO, "&Undo\tCtrl+Z");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_CUT, "Cu&t\tCtrl+X");
    AppendMenuA(m, MF_STRING, IDM_COPY, "&Copy\tCtrl+C");
    AppendMenuA(m, MF_STRING, IDM_PASTE, "&Paste\tCtrl+V");
    AppendMenuA(m, MF_STRING, IDM_SELALL, "Select &All\tCtrl+A");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)m, "&Edit");
    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, IDM_ASM, "&Assemble\tF7");
    AppendMenuA(m, MF_STRING, IDM_RUN, "&Run on Kit\tF5");
    AppendMenuA(m, MF_STRING, IDM_SENDABS, "Send .&ABS file...");
    AppendMenuA(m, MF_STRING, IDM_STOP, "&Stop\tEsc");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_FIND, "&Find kit (connect)");
    AppendMenuA(m, MF_STRING, IDM_CLEAR, "&Clear output");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)m, "&Kit");
    m = CreatePopupMenu();                /* built into the program: no folder needed */
    for (i = 0; i < kit_nexamples; i++)
        AppendMenuA(m, MF_STRING, IDM_EX_FIRST + i, kit_examples[i].menu);
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)m, "E&xamples");
    m = CreatePopupMenu();
    for (i = 0; i < kit_ntemplates; i++)
        AppendMenuA(m, MF_STRING, IDM_TPL_FIRST + i, kit_templates[i].menu);
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)m, "&Templates");
    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, IDM_HELP, "&How to use\tF1");
    AppendMenuA(m, MF_STRING, IDM_HELP_CONN, "&Connecting the kit");
    AppendMenuA(m, MF_STRING, IDM_KITLOG, "Kit &log");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_ABOUT, "&About");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)m, "&Help");
    return bar;
}

static void create_children(HWND hwnd)
{
    NONCLIENTMETRICSA ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        g_fontUI = CreateFontIndirectA(&ncm.lfMessageFont);
    else
        g_fontUI = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_fontMono = make_mono_font(10);

    g_btnAsm = make_button("Assemble (F7)", IDC_ASM);
    g_btnRun = make_button("Run on Kit (F5)", IDC_RUN);
    g_btnStop = make_button("Stop", IDC_STOP);
    g_portLbl = CreateWindowExA(0, "STATIC", "Port:", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                                (HMENU)IDC_PORTLBL, g_inst, NULL);
    SendMessageA(g_portLbl, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
    g_port = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                             CBS_DROPDOWNLIST, 0, 0, 10, 200, hwnd, (HMENU)IDC_PORT, g_inst, NULL);
    SendMessageA(g_port, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
    g_btnRefresh = make_button("Refresh", IDC_REFRESH);

    g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                             ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                             0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT, g_inst, NULL);
    SendMessageA(g_edit, WM_SETFONT, (WPARAM)g_fontMono, FALSE);
    SendMessageA(g_edit, EM_SETLIMITTEXT, 2 * 1024 * 1024, 0);
    {
        int tabs = 32;   /* 8 characters, like Notepad and the lab samples */
        SendMessageA(g_edit, EM_SETTABSTOPS, 1, (LPARAM)&tabs);
    }

    g_out = CreateWindowExA(WS_EX_CLIENTEDGE, "RichEdit20A", "",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                            ES_AUTOVSCROLL | ES_NOHIDESEL,
                            0, 0, 10, 10, hwnd, (HMENU)IDC_OUT, g_inst, NULL);
    if (!g_out)      /* very old system without RichEdit 2: plain edit box */
        g_out = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd, (HMENU)IDC_OUT, g_inst, NULL);
    SendMessageA(g_out, WM_SETFONT, (WPARAM)g_fontMono, FALSE);
    SendMessageA(g_out, EM_EXLIMITTEXT, 0, 1024 * 1024);
    SendMessageA(g_out, EM_SETEVENTMASK, 0, ENM_MOUSEEVENTS);

    g_cmd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE,
                            0, 0, 10, 10, hwnd, (HMENU)IDC_CMD, g_inst, NULL);
    SendMessageA(g_cmd, WM_SETFONT, (WPARAM)g_fontMono, FALSE);
    SendMessageA(g_cmd, EM_SETLIMITTEXT, 120, 0);
    g_cmdOldProc = (WNDPROC)SetWindowLongPtrA(g_cmd, GWLP_WNDPROC, (LONG_PTR)cmd_proc);
    g_btnSend = make_button("Send to kit", IDC_SEND);

    g_status = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, g_inst, NULL);
    SendMessageA(g_status, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
}

/* ------------------------------------------------------------------ window procedure */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        create_children(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, 1, 150, NULL);
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(560);
        mm->ptMinTrackSize.y = S(400);
        return 0;
    }

    case WM_TIMER:
        update_line_col();
        return 0;

    case WM_KITTEXT:
        out_msg((int)wp, (const char *)lp);
        free((void *)lp);
        return 0;

    case WM_KITSTATE:
        set_busy_ui((int)wp, (const char *)lp);
        free((void *)lp);
        return 0;

    case WM_DROPFILES: {
        char file[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFileA(drop, 0, file, sizeof file)) {
            const char *ext = strrchr(file, '.');
            if (ext && _stricmp(ext, ".abs") == 0) {
                char *text = read_text_file(file), why[200];
                int n = 0;
                if (text && kit_check_abs(text, &n, why, sizeof why) > 0) {
                    outf(KO_INFO, "---- Sending %s (%d bytes) ----", file_part(file), n);
                    start_job(JOB_RUN, text, 1);
                } else {
                    outf(KO_ERR, "%s is not a valid .ABS program file.", file_part(file));
                }
                free(text);
            } else if (ask_save_changes()) {
                open_path(file);
            }
        }
        DragFinish(drop);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_OUT && nh->code == EN_MSGFILTER) {
            MSGFILTER *mf = (MSGFILTER *)lp;
            if (mf->msg == WM_LBUTTONDBLCLK) {
                POINTL pt;
                int ch, line;
                char buf[600];
                pt.x = (short)LOWORD(mf->lParam);
                pt.y = (short)HIWORD(mf->lParam);
                ch = (int)SendMessageA(g_out, EM_CHARFROMPOS, 0, (LPARAM)&pt);
                line = (int)SendMessageA(g_out, EM_EXLINEFROMCHAR, 0, ch);
                *(WORD *)buf = (WORD)(sizeof buf - 2);
                ch = (int)SendMessageA(g_out, EM_GETLINE, line, (LPARAM)buf);
                if (ch > 0 && ch < (int)sizeof buf) {
                    buf[ch] = 0;
                    goto_error_line(buf);
                }
                return 1;
            }
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_EDIT:
            if (code == EN_CHANGE) update_title();
            return 0;
        case IDC_PORT:
            if (code == CBN_SELCHANGE) port_changed();
            return 0;
        case IDC_REFRESH: fill_ports(); return 0;
        case IDM_NEW:
            if (ask_save_changes()) set_editor_text("", NULL, NULL, NULL);
            return 0;
        case IDM_OPEN: do_open(); return 0;
        case IDM_SAVE: do_save(); return 0;
        case IDM_SAVEAS: save_as(); return 0;
        case IDM_EXIT: SendMessageA(hwnd, WM_CLOSE, 0, 0); return 0;
        case IDM_UNDO: SendMessageA(GetFocus(), EM_UNDO, 0, 0); return 0;
        case IDM_CUT: SendMessageA(GetFocus(), WM_CUT, 0, 0); return 0;
        case IDM_COPY: SendMessageA(GetFocus(), WM_COPY, 0, 0); return 0;
        case IDM_PASTE: SendMessageA(GetFocus(), WM_PASTE, 0, 0); return 0;
        case IDM_SELALL: {
            HWND f = GetFocus();
            if (f == g_out) { CHARRANGE cr = { 0, -1 }; SendMessageA(g_out, EM_EXSETSEL, 0, (LPARAM)&cr); }
            else SendMessageA(f == g_cmd ? g_cmd : g_edit, EM_SETSEL, 0, -1);
            return 0;
        }
        case IDC_ASM: case IDM_ASM: { char *a = assemble(0); free(a); return 0; }
        case IDC_RUN: case IDM_RUN: do_run(); return 0;
        case IDM_SENDABS: do_send_abs(); return 0;
        case IDC_STOP: case IDM_STOP:
            if (g_busy) { InterlockedExchange(&g_cancel, 1); outf(KO_WARN, "Stopping ..."); }
            return 0;
        case IDM_FIND: start_job(JOB_FIND, NULL, 0); return 0;
        case IDM_CLEAR: SetWindowTextA(g_out, ""); g_outAtLineStart = 1; return 0;
        case IDC_SEND: {
            char line[200];
            GetWindowTextA(g_cmd, line, sizeof line);
            if (!line[0]) { SetFocus(g_cmd); return 0; }
            if (start_job(JOB_CMD, line, 0)) SetWindowTextA(g_cmd, "");
            SetFocus(g_cmd);
            return 0;
        }
        case IDM_HELP: show_text_box("How to use - " APP_NAME, KITUI_HELP); return 0;
        case IDM_HELP_CONN: show_text_box("Connecting the kit", KITUI_CONN); return 0;
        case IDM_KITLOG: {
            char arg[MAX_PATH + 24];
            if (GetFileAttributesA(g_logPath) == INVALID_FILE_ATTRIBUTES) {
                outf(KO_INFO, "No kit log yet.");
                return 0;
            }
            outf(KO_INFO, "Kit log: %s", g_logPath);
            snprintf(arg, sizeof arg, "\"%s\"", g_logPath);
            ShellExecuteA(hwnd, "open", "notepad.exe", arg, NULL, SW_SHOWNORMAL);
            return 0;
        }
        case IDM_ABOUT: show_text_box("About", KITUI_ABOUT); return 0;
        default:
            if (id >= IDM_EX_FIRST && id < IDM_EX_FIRST + kit_nexamples) {
                load_builtin(&kit_examples[id - IDM_EX_FIRST], 0);
                return 0;
            }
            if (id >= IDM_TPL_FIRST && id < IDM_TPL_FIRST + kit_ntemplates) {
                load_builtin(&kit_templates[id - IDM_TPL_FIRST], 1);
                return 0;
            }
            break;
        }
        break;
    }

    case WM_CLOSE:
        if (!ask_save_changes()) return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        InterlockedExchange(&g_cancel, 1);
        InterlockedExchange(&g_quit, 1);
        SetEvent(g_wake);
        if (g_thread) WaitForSingleObject(g_thread, 3000);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEXA wc;
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_NEW }, { FVIRTKEY | FCONTROL, 'O', IDM_OPEN },
        { FVIRTKEY | FCONTROL, 'S', IDM_SAVE }, { FVIRTKEY | FCONTROL, 'A', IDM_SELALL },
        { FVIRTKEY, VK_F5, IDM_RUN }, { FVIRTKEY, VK_F7, IDM_ASM },
        { FVIRTKEY, VK_ESCAPE, IDM_STOP }, { FVIRTKEY, VK_F1, IDM_HELP },
    };
    HDC dc;
    DWORD n;
    (void)prev; (void)cmdline;
    g_inst = inst;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    LoadLibraryA("riched20.dll");
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;

    n = GetTempPathA(sizeof g_tmpdir - 20, g_tmpdir);
    if (n == 0 || n > sizeof g_tmpdir - 20) strcpy(g_tmpdir, ".\\");
    strcat(g_tmpdir, "KitStudio\\");
    CreateDirectoryA(g_tmpdir, NULL);
    snprintf(g_logPath, sizeof g_logPath, "%skit_log.txt", g_tmpdir);

    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APP));
    wc.hIconSm = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "KitStudioMain";
    RegisterClassExA(&wc);

    g_main = CreateWindowExA(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(940), S(680), NULL, make_menu(), inst, NULL);
    if (!g_main) return 1;
    g_accel = CreateAcceleratorTableA(acc, sizeof acc / sizeof acc[0]);

    set_editor_text("", NULL, NULL, NULL); /* always start with an empty editor */
    outf(KO_INFO, APP_NAME " " APP_VERSION " - write a program, press F7 to assemble, F5 to run it on the kit.");
    outf(KO_INFO, "Kit: SELECT MODE switch = PC. If it does not answer, press RESET on the kit. Help: F1.");
    fill_ports();
    set_busy_ui(0, NULL);
    if (__argc > 1) open_path(__argv[1]);

    g_thread = (HANDLE)_beginthreadex(NULL, 0, worker, NULL, 0, NULL);

    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    SetFocus(g_edit);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (TranslateAcceleratorA(g_main, g_accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
