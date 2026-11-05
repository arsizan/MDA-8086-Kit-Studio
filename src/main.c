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
    IDC_SIMVIEW, IDC_SIMRUN, IDC_SIMSTEP, IDC_SIMPAUSE, IDC_SIMRESET, IDC_SPEED, IDC_SPEEDLBL,
    IDM_NEW = 200, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_EXIT,
    IDM_UNDO, IDM_CUT, IDM_COPY, IDM_PASTE, IDM_SELALL,
    IDM_ASM, IDM_RUN, IDM_SENDABS, IDM_STOP, IDM_FIND, IDM_CLEAR,
    IDM_HELP, IDM_HELP_CONN, IDM_KITLOG, IDM_ABOUT,
    IDM_SIMRUN, IDM_SIMSTEP, IDM_SIMPAUSE, IDM_SIMRESET, IDM_SIMPANEL, IDM_SIMFLOAT,
    IDM_EX_FIRST = 300,             /* 300.. : built-in examples (kit_examples[]) */
    IDM_TPL_FIRST = 340             /* 340.. : built-in templates (kit_templates[]) */
};

enum { JOB_NONE, JOB_RUN, JOB_CMD, JOB_FIND, JOB_PORT };

/* ------------------------------------------------------------------ globals */
static HINSTANCE g_inst;
static HWND g_main, g_edit, g_out, g_cmd, g_status, g_port, g_btnAsm, g_btnRun, g_btnStop,
            g_btnRefresh, g_btnSend, g_portLbl;
/* Simulator panel (right side) */
static HWND g_simView, g_btnSimRun, g_btnSimStep, g_btnSimPause, g_btnSimReset, g_speed, g_speedLbl;
static KitSimCtl g_sim;
static int g_simShown = 1, g_speedIdx = 6;
static HWND g_simWin;              /* Simulator > Separate window: the panel in its own window */
static int g_simFloat, g_narrow;   /* g_narrow: screen too small for both side by side */
static DWORD g_simTick;
static const double SPEEDS[7] = { 1.0 / 1000, 1.0 / 300, 1.0 / 100, 1.0 / 30, 1.0 / 10, 1.0 / 3, 1.0 };
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

/* assembles the editor text; on success returns malloc'd .ABS text (prog: for the simulator) */
static char *assemble_ex(int quiet_dump, KitUiProgram *prog)
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
                         text, g_tmpdir, g_path[0] ? g_path : NULL, quiet_dump, ui_out, prog);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
    free(text);
    return abs;
}

static char *assemble(int quiet_dump) { return assemble_ex(quiet_dump, NULL); }

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

static void layout(void);
/* ------------------------------------------------------------------ simulator panel */
/* GDI+ draws smooth circles and shapes (Windows XP and later have it); without it, plain GDI */
typedef struct { UINT32 ver; void *cb; BOOL nothread, nocodecs; } GpInput;
typedef int (WINAPI *GpStartup_t)(ULONG_PTR *, const GpInput *, void *);
typedef int (WINAPI *GpFromHDC_t)(HDC, void **);
typedef int (WINAPI *GpOne_t)(void *);
typedef int (WINAPI *GpSmoothing_t)(void *, int);
typedef int (WINAPI *GpSolidFill_t)(DWORD, void **);
typedef int (WINAPI *GpPen_t)(DWORD, float, int, void **);
typedef int (WINAPI *GpEllipse_t)(void *, void *, float, float, float, float);
typedef int (WINAPI *GpPolygon_t)(void *, void *, const float *, int, int);
typedef int (WINAPI *GpPath_t)(int, void **);
typedef int (WINAPI *GpArc_t)(void *, float, float, float, float, float, float);
typedef int (WINAPI *GpPathOp_t)(void *, void *, void *);
typedef int (WINAPI *GpFlush_t)(void *, int);
static struct {
    int ok;
    GpFromHDC_t FromHDC;
    GpOne_t DelGraphics, DelBrush, DelPen, DelPath, ClosePath;
    GpSmoothing_t Smoothing;
    GpSolidFill_t SolidFill;
    GpPen_t Pen;
    GpEllipse_t FillEllipse, DrawEllipse;
    GpPolygon_t FillPolygon;
    GpPath_t Path;
    GpArc_t Arc;
    GpPathOp_t FillPath, DrawPath;
    GpFlush_t Flush;
} gp;
static char g_monoFace[32] = "Courier New";

static void load_gdiplus(void)
{
    HMODULE h = LoadLibraryA("gdiplus.dll");
    GpStartup_t start;
    GpInput in;
    ULONG_PTR token;
    if (!h) return;
    start = (GpStartup_t)GetProcAddress(h, "GdiplusStartup");
    gp.FromHDC = (GpFromHDC_t)GetProcAddress(h, "GdipCreateFromHDC");
    gp.DelGraphics = (GpOne_t)GetProcAddress(h, "GdipDeleteGraphics");
    gp.DelBrush = (GpOne_t)GetProcAddress(h, "GdipDeleteBrush");
    gp.DelPen = (GpOne_t)GetProcAddress(h, "GdipDeletePen");
    gp.DelPath = (GpOne_t)GetProcAddress(h, "GdipDeletePath");
    gp.ClosePath = (GpOne_t)GetProcAddress(h, "GdipClosePathFigure");
    gp.Smoothing = (GpSmoothing_t)GetProcAddress(h, "GdipSetSmoothingMode");
    gp.SolidFill = (GpSolidFill_t)GetProcAddress(h, "GdipCreateSolidFill");
    gp.Pen = (GpPen_t)GetProcAddress(h, "GdipCreatePen1");
    gp.FillEllipse = (GpEllipse_t)GetProcAddress(h, "GdipFillEllipse");
    gp.DrawEllipse = (GpEllipse_t)GetProcAddress(h, "GdipDrawEllipse");
    gp.FillPolygon = (GpPolygon_t)GetProcAddress(h, "GdipFillPolygon");
    gp.Path = (GpPath_t)GetProcAddress(h, "GdipCreatePath");
    gp.Arc = (GpArc_t)GetProcAddress(h, "GdipAddPathArc");
    gp.FillPath = (GpPathOp_t)GetProcAddress(h, "GdipFillPath");
    gp.DrawPath = (GpPathOp_t)GetProcAddress(h, "GdipDrawPath");
    gp.Flush = (GpFlush_t)GetProcAddress(h, "GdipFlush");
    if (!start || !gp.FromHDC || !gp.DelGraphics || !gp.DelBrush || !gp.DelPen || !gp.DelPath ||
        !gp.ClosePath || !gp.Smoothing || !gp.SolidFill || !gp.Pen || !gp.FillEllipse ||
        !gp.DrawEllipse || !gp.FillPolygon || !gp.Path || !gp.Arc || !gp.FillPath || !gp.DrawPath || !gp.Flush)
        return;
    memset(&in, 0, sizeof in);
    in.ver = 1;
    if (start(&token, &in, NULL) == 0) gp.ok = 1;
}

typedef struct { HDC dc; void *g; } WinDraw;
static DWORD argb(unsigned c) { return 0xFF000000u | (c & 0xFFFFFF); }
static COLORREF cref(unsigned c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

static void wd_rect(void *ctx, double x, double y, double w, double h, double r, unsigned fill, unsigned line)
{
    WinDraw *d = (WinDraw *)ctx;
    if (d->g && r > 0.5) {
        void *path = NULL, *br = NULL, *pen = NULL;
        float fx = (float)x + 0.5f, fy = (float)y + 0.5f, fw = (float)w - 1, fh = (float)h - 1, rr = (float)r * 2;
        gp.Path(0, &path);
        gp.Arc(path, fx, fy, rr, rr, 180, 90);
        gp.Arc(path, fx + fw - rr, fy, rr, rr, 270, 90);
        gp.Arc(path, fx + fw - rr, fy + fh - rr, rr, rr, 0, 90);
        gp.Arc(path, fx, fy + fh - rr, rr, rr, 90, 90);
        gp.ClosePath(path);
        if (fill != KV_NONE && gp.SolidFill(argb(fill), &br) == 0) { gp.FillPath(d->g, br, path); gp.DelBrush(br); }
        if (line != KV_NONE && gp.Pen(argb(line), 1.0f, 2, &pen) == 0) { gp.DrawPath(d->g, pen, path); gp.DelPen(pen); }
        gp.DelPath(path);
        return;
    }
    {
        HBRUSH b = fill != KV_NONE ? CreateSolidBrush(cref(fill)) : (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN p = line != KV_NONE ? CreatePen(PS_SOLID, 1, cref(line)) : (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ ob = SelectObject(d->dc, b), op = SelectObject(d->dc, p);
        int ix = (int)(x + 0.5), iy = (int)(y + 0.5), ir = (int)(x + w + 0.5) + (line == KV_NONE), ib = (int)(y + h + 0.5) + (line == KV_NONE);
        if (d->g) gp.Flush(d->g, 0);
        if (r > 0.5) RoundRect(d->dc, ix, iy, ir, ib, (int)(r * 2), (int)(r * 2));
        else Rectangle(d->dc, ix, iy, ir, ib);
        SelectObject(d->dc, ob);
        SelectObject(d->dc, op);
        if (fill != KV_NONE) DeleteObject(b);
        if (line != KV_NONE) DeleteObject(p);
    }
}

static void wd_circle(void *ctx, double cx, double cy, double r, unsigned fill, unsigned line, double width)
{
    WinDraw *d = (WinDraw *)ctx;
    if (d->g) {
        void *br = NULL, *pen = NULL;
        if (fill != KV_NONE && gp.SolidFill(argb(fill), &br) == 0) {
            gp.FillEllipse(d->g, br, (float)(cx - r), (float)(cy - r), (float)(2 * r), (float)(2 * r));
            gp.DelBrush(br);
        }
        if (line != KV_NONE && gp.Pen(argb(line), (float)width, 2, &pen) == 0) {
            gp.DrawEllipse(d->g, pen, (float)(cx - r), (float)(cy - r), (float)(2 * r), (float)(2 * r));
            gp.DelPen(pen);
        }
        return;
    }
    {
        HBRUSH b = fill != KV_NONE ? CreateSolidBrush(cref(fill)) : (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN p = line != KV_NONE ? CreatePen(PS_SOLID, width > 1.5 ? 2 : 1, cref(line)) : (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ ob = SelectObject(d->dc, b), op = SelectObject(d->dc, p);
        Ellipse(d->dc, (int)(cx - r + 0.5), (int)(cy - r + 0.5), (int)(cx + r + 0.5), (int)(cy + r + 0.5));
        SelectObject(d->dc, ob);
        SelectObject(d->dc, op);
        if (fill != KV_NONE) DeleteObject(b);
        if (line != KV_NONE) DeleteObject(p);
    }
}

static void wd_poly(void *ctx, const double *xy, int n, unsigned fill)
{
    WinDraw *d = (WinDraw *)ctx;
    int i;
    if (d->g) {
        float pts[16];
        void *br = NULL;
        for (i = 0; i < n && i < 8; i++) { pts[2 * i] = (float)xy[2 * i]; pts[2 * i + 1] = (float)xy[2 * i + 1]; }
        if (gp.SolidFill(argb(fill), &br) == 0) { gp.FillPolygon(d->g, br, pts, n, 0); gp.DelBrush(br); }
        return;
    }
    {
        POINT pt[8];
        HBRUSH b = CreateSolidBrush(cref(fill));
        HGDIOBJ ob = SelectObject(d->dc, b), op = SelectObject(d->dc, GetStockObject(NULL_PEN));
        for (i = 0; i < n && i < 8; i++) { pt[i].x = (LONG)(xy[2 * i] + 0.5); pt[i].y = (LONG)(xy[2 * i + 1] + 0.5); }
        Polygon(d->dc, pt, n < 8 ? n : 8);
        SelectObject(d->dc, ob);
        SelectObject(d->dc, op);
        DeleteObject(b);
    }
}

static HFONT view_font(double size, int flags)
{
    static struct { int key; HFONT f; } cache[32];
    static int n;
    int px = (int)(size + 0.5), i, key;
    if (px < 6) px = 6;
    key = px | ((flags & KV_BOLD) ? 0x10000 : 0) | ((flags & KV_MONO) ? 0x20000 : 0);
    for (i = 0; i < n; i++) if (cache[i].key == key) return cache[i].f;
    if (n == 32) { for (i = 0; i < n; i++) DeleteObject(cache[i].f); n = 0; }
    cache[n].key = key;
    cache[n].f = CreateFontA(-px, 0, 0, 0, (flags & KV_BOLD) ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             (flags & KV_MONO) ? (FIXED_PITCH | FF_MODERN) : (VARIABLE_PITCH | FF_SWISS),
                             (flags & KV_MONO) ? g_monoFace : "Segoe UI");
    return cache[n++].f;
}

static void wd_text(void *ctx, double x, double y, const char *s, double size, int flags, unsigned color)
{
    WinDraw *d = (WinDraw *)ctx;
    HGDIOBJ of;
    int n = (int)strlen(s);
    if (d->g) gp.Flush(d->g, 0);
    of = SelectObject(d->dc, view_font(size, flags));
    SetBkMode(d->dc, TRANSPARENT);
    SetTextColor(d->dc, cref(color));
    if (flags & (KV_CENTER | KV_RIGHT)) {
        SIZE sz;
        GetTextExtentPoint32A(d->dc, s, n, &sz);
        x -= (flags & KV_CENTER) ? sz.cx / 2.0 : sz.cx;
    }
    TextOutA(d->dc, (int)(x + 0.5), (int)(y + 0.5), s, n);
    SelectObject(d->dc, of);
}

static LRESULT CALLBACK simview_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(h, &ps), mem;
        HBITMAP bmp;
        HGDIOBJ old;
        WinDraw d;
        KitDraw kd;
        GetClientRect(h, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            mem = CreateCompatibleDC(dc);
            bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            old = SelectObject(mem, bmp);
            d.dc = mem;
            d.g = NULL;
            if (gp.ok && gp.FromHDC(mem, &d.g) == 0) gp.Smoothing(d.g, 4);   /* anti-aliased */
            else d.g = NULL;
            kd.ctx = &d;
            kd.rect = wd_rect;
            kd.circle = wd_circle;
            kd.poly = wd_poly;
            kd.text = wd_text;
            kitview_draw(&kd, rc.right, rc.bottom, &g_sim.panel);
            if (d.g) { gp.Flush(d.g, 0); gp.DelGraphics(d.g); }
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static char *assemble_ex(int quiet_dump, KitUiProgram *prog);

/* refresh the panel; when the program does not run, select the next line in the editor */
static void sim_show(void)
{
    kitsim_update(&g_sim);
    InvalidateRect(g_simView, NULL, FALSE);
    EnableWindow(g_btnSimPause, g_sim.panel.running);
    if (!g_sim.panel.running && g_sim.panel.line > 0) {
        int start = (int)SendMessageA(g_edit, EM_LINEINDEX, g_sim.panel.line - 1, 0);
        if (start >= 0) {
            int len = (int)SendMessageA(g_edit, EM_LINELENGTH, start, 0);
            SendMessageA(g_edit, EM_SETSEL, start, start + len);
            SendMessageA(g_edit, EM_SCROLLCARET, 0, 0);
        }
    }
}

/* the editor's program in the simulator: assembled again only when the text changed */
static int sim_prepare(int *fresh)
{
    char *text = editor_text(), *abs;
    KitUiProgram prog;
    *fresh = 0;
    if (text && kitsim_same_source(&g_sim, text)) { free(text); return 1; }
    free(text);
    memset(&prog, 0, sizeof prog);
    abs = assemble_ex(1, &prog);
    if (!abs) { kitui_free_program(&prog); return 0; }
    free(abs);
    text = editor_text();
    kitsim_load(&g_sim, &prog, text ? text : "");
    free(text);
    outf(KO_INFO, "Simulator: program loaded (%d bytes at 0000:1000).", g_sim.prog.len);
    *fresh = 1;
    return 1;
}

static void sim_run(void)
{
    int fresh;
    if (!sim_prepare(&fresh)) return;
    if (g_sim.panel.v.stop >= 0) kitsim_reset(&g_sim);     /* it had stopped: start again */
    g_sim.panel.running = 1;
    g_simTick = GetTickCount();
    SetTimer(g_main, 2, 30, NULL);
    sim_show();
}

static void sim_step(void)
{
    int fresh;
    if (!sim_prepare(&fresh)) return;
    KillTimer(g_main, 2);
    g_sim.panel.running = 0;
    if (!fresh) {                       /* just loaded: first show where it starts */
        if (g_sim.panel.v.stop >= 0) kitsim_reset(&g_sim);
        else kitsim_step(&g_sim);
    }
    sim_show();
}

static void sim_pause(void)
{
    if (!g_sim.panel.running) return;
    g_sim.panel.running = 0;
    KillTimer(g_main, 2);
    sim_show();
}

static void sim_reset(void)
{
    KillTimer(g_main, 2);
    kitsim_reset(&g_sim);
    sim_show();
}

static void sim_speed_changed(void)
{
    char buf[40];
    g_speedIdx = (int)SendMessageA(g_speed, TBM_GETPOS, 0, 0);
    if (g_speedIdx < 0) g_speedIdx = 0;
    if (g_speedIdx > 6) g_speedIdx = 6;
    g_sim.panel.speed = SPEEDS[g_speedIdx];
    if (g_speedIdx == 6) snprintf(buf, sizeof buf, "Speed 1x");
    else snprintf(buf, sizeof buf, "Speed 1/%.0fx", 1.0 / SPEEDS[g_speedIdx]);
    SetWindowTextA(g_speedLbl, buf);
    InvalidateRect(g_simView, NULL, FALSE);
}

static void sim_float(int on);

static void sim_panel_toggle(void)
{
    int sw;
    if (g_simFloat) sim_float(0);       /* back in the main window first */
    g_simShown = !g_simShown;
    sw = g_simShown ? SW_SHOW : SW_HIDE;
    ShowWindow(g_simView, sw);
    ShowWindow(g_btnSimRun, sw);
    ShowWindow(g_btnSimStep, sw);
    ShowWindow(g_btnSimPause, sw);
    ShowWindow(g_btnSimReset, sw);
    ShowWindow(g_speed, sw);
    ShowWindow(g_speedLbl, sw);
    CheckMenuItem(GetMenu(g_main), IDM_SIMPANEL, g_simShown ? MF_CHECKED : MF_UNCHECKED);
    layout();
    InvalidateRect(g_main, NULL, TRUE);
}

/* the simulator's buttons in a row at (simX, y), the picture below them down to 'bottom' */
static void sim_layout(int simX, int y, int simW, int bottom)
{
    int m = S(6), bh = S(26), x = simX;
    MoveWindow(g_btnSimRun, x, y, S(64), bh, TRUE); x += S(64) + S(4);
    MoveWindow(g_btnSimStep, x, y, S(64), bh, TRUE); x += S(64) + S(4);
    MoveWindow(g_btnSimPause, x, y, S(70), bh, TRUE); x += S(70) + S(4);
    MoveWindow(g_btnSimReset, x, y, S(52), bh, TRUE); x += S(52) + S(8);
    {
        int lw = S(86), tw = simX + simW - x - lw;
        if (tw < S(70)) tw = S(70);
        MoveWindow(g_speed, x, y, tw, bh, TRUE);
        MoveWindow(g_speedLbl, x + tw + S(2), y + S(5), lw - S(2), bh - S(5), TRUE);
    }
    MoveWindow(g_simView, simX, y + bh + m, simW, bottom - (y + bh + m), TRUE);
}

/* the separate simulator window: its buttons talk to the main window */
static LRESULT CALLBACK simwin_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_simFloat) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            sim_layout(S(6), S(6), rc.right - S(12), rc.bottom - S(6));
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(420);
        mm->ptMinTrackSize.y = S(360);
        return 0;
    }
    case WM_COMMAND:
    case WM_HSCROLL:
        return SendMessageA(g_main, msg, wp, lp);
    case WM_CLOSE:
        sim_float(0);                   /* closing it puts the simulator back into the main window */
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void sim_float(int on)
{
    HWND kids[7];
    int i;
    if (on == g_simFloat) return;
    kids[0] = g_btnSimRun; kids[1] = g_btnSimStep; kids[2] = g_btnSimPause; kids[3] = g_btnSimReset;
    kids[4] = g_speed; kids[5] = g_speedLbl; kids[6] = g_simView;
    if (on) {
        RECT wa, rc;
        int w = S(600), h = S(720);
        if (!g_simShown) sim_panel_toggle();
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) { wa.left = wa.top = 0; wa.right = 1024; wa.bottom = 740; }
        if (w > wa.right - wa.left) w = wa.right - wa.left;
        if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
        /* owned by the main window: it stays above the editor, like emu8086's emulator window */
        g_simWin = CreateWindowExA(0, "KitSimWin", "Simulator - " APP_NAME, WS_OVERLAPPEDWINDOW,
                                   wa.right - w, wa.top, w, h, g_main, NULL, g_inst, NULL);
        if (!g_simWin) return;
        for (i = 0; i < 7; i++) SetParent(kids[i], g_simWin);
        g_simFloat = 1;
        GetClientRect(g_simWin, &rc);
        sim_layout(S(6), S(6), rc.right - S(12), rc.bottom - S(6));
        ShowWindow(g_simWin, SW_SHOWNOACTIVATE);
    } else {
        g_simFloat = 0;
        for (i = 0; i < 7; i++) SetParent(kids[i], g_main);
        if (g_simWin) DestroyWindow(g_simWin);
        g_simWin = NULL;
    }
    CheckMenuItem(GetMenu(g_main), IDM_SIMFLOAT, g_simFloat ? MF_CHECKED : MF_UNCHECKED);
    layout();
    InvalidateRect(g_main, NULL, TRUE);
}

/* ------------------------------------------------------------------ layout */
static void layout(void)
{
    RECT rc, sr;
    int m = S(6), bh = S(26), x, y, w, h, sh, outH, cmdH = S(24), top, simW = 0, simX;
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
    if (g_simShown && !g_simFloat) {    /* the simulator takes the right part of the window */
        simW = (rc.right - 3 * m) * 45 / 100;
        if (simW > rc.right - 3 * m - S(560)) simW = rc.right - 3 * m - S(560);   /* editor side first */
        if (simW < S(360)) simW = S(360);
    }
    w = rc.right - 2 * m - (simW ? simW + m : 0);
    x = m; y = m;
    {   /* narrow editor side: smaller buttons, so nothing runs into the simulator's buttons */
        int c = w < S(600);
        MoveWindow(g_btnAsm, x, y, c ? S(100) : S(110), bh, TRUE); x += (c ? S(100) : S(110)) + m;
        MoveWindow(g_btnRun, x, y, c ? S(112) : S(130), bh, TRUE); x += (c ? S(112) : S(130)) + m;
        MoveWindow(g_btnStop, x, y, c ? S(48) : S(60), bh, TRUE); x += (c ? S(48) : S(60)) + (c ? S(10) : S(18));
        MoveWindow(g_portLbl, x, y + S(5), S(32), bh - S(5), TRUE); x += S(34);
        MoveWindow(g_port, x, y + S(1), c ? S(118) : S(140), S(200), TRUE); x += (c ? S(118) : S(140)) + m;
        MoveWindow(g_btnRefresh, x, y, c ? S(62) : S(70), bh, TRUE);
    }
    top = y + bh + m;
    h = rc.bottom - sh - top;
    outH = h * 34 / 100;
    if (outH < S(90)) outH = S(90);
    MoveWindow(g_edit, m, top, w, h - outH - cmdH - 2 * m, TRUE);
    MoveWindow(g_out, m, top + h - outH - cmdH - m, w, outH, TRUE);
    MoveWindow(g_cmd, m, top + h - cmdH - m / 2, w - S(90) - m, cmdH, TRUE);
    MoveWindow(g_btnSend, m + w - S(90), top + h - cmdH - m / 2, S(90), cmdH, TRUE);
    if (simW) {
        simX = m + w + m;
        sim_layout(simX, y, simW, rc.bottom - sh - m / 2);
    }
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
    snprintf(g_monoFace, sizeof g_monoFace, "%s", lf.lfFaceName);
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
    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, IDM_SIMRUN, "&Run in simulator\tF6");
    AppendMenuA(m, MF_STRING, IDM_SIMSTEP, "&Step (one instruction)\tF8");
    AppendMenuA(m, MF_STRING, IDM_SIMPAUSE, "&Pause\tF9");
    AppendMenuA(m, MF_STRING, IDM_SIMRESET, "R&eset");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | (g_simShown ? MF_CHECKED : 0), IDM_SIMPANEL, "Show simulator &panel");
    AppendMenuA(m, MF_STRING, IDM_SIMFLOAT, "Separate &window");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)m, "&Simulator");
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

    g_btnSimRun = make_button("Run F6", IDC_SIMRUN);
    g_btnSimStep = make_button("Step F8", IDC_SIMSTEP);
    g_btnSimPause = make_button("Pause F9", IDC_SIMPAUSE);
    g_btnSimReset = make_button("Reset", IDC_SIMRESET);
    EnableWindow(g_btnSimPause, FALSE);
    g_speed = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                              0, 0, 10, 10, hwnd, (HMENU)IDC_SPEED, g_inst, NULL);
    SendMessageA(g_speed, TBM_SETRANGE, TRUE, MAKELPARAM(0, 6));
    SendMessageA(g_speed, TBM_SETPOS, TRUE, g_speedIdx);
    g_speedLbl = CreateWindowExA(0, "STATIC", "Speed 1x", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                                 (HMENU)IDC_SPEEDLBL, g_inst, NULL);
    SendMessageA(g_speedLbl, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
    g_simView = CreateWindowExA(0, "KitSimView", "", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                                (HMENU)IDC_SIMVIEW, g_inst, NULL);

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
        mm->ptMinTrackSize.x = S(640);
        mm->ptMinTrackSize.y = S(480);
        return 0;
    }

    case WM_TIMER:
        if (wp == 2) {                  /* simulator running */
            DWORD now = GetTickCount();
            kitsim_tick(&g_sim, (now - g_simTick) / 1000.0);
            g_simTick = now;
            if (!g_sim.panel.running) KillTimer(hwnd, 2);
            sim_show();
            return 0;
        }
        update_line_col();
        return 0;

    case WM_HSCROLL:
        if ((HWND)lp == g_speed) sim_speed_changed();
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
        case IDC_SIMRUN: case IDM_SIMRUN: sim_run(); return 0;
        case IDC_SIMSTEP: case IDM_SIMSTEP: sim_step(); return 0;
        case IDC_SIMPAUSE: case IDM_SIMPAUSE: sim_pause(); return 0;
        case IDC_SIMRESET: case IDM_SIMRESET: sim_reset(); return 0;
        case IDM_SIMPANEL: sim_panel_toggle(); return 0;
        case IDM_SIMFLOAT: sim_float(!g_simFloat); return 0;
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
        KillTimer(hwnd, 2);
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
        { FVIRTKEY, VK_F6, IDM_SIMRUN }, { FVIRTKEY, VK_F8, IDM_SIMSTEP }, { FVIRTKEY, VK_F9, IDM_SIMPAUSE },
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
    wc.lpfnWndProc = simview_proc;              /* the Simulator panel's picture */
    wc.hIcon = NULL;
    wc.hIconSm = NULL;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "KitSimView";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = simwin_proc;               /* Simulator > Separate window */
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APP));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "KitSimWin";
    RegisterClassExA(&wc);
    load_gdiplus();
    kitsim_unload(&g_sim);
    g_sim.panel.speed = 1.0;

    {   /* editor and simulator side by side: as wide as the screen allows */
        RECT wa;
        int ww = S(1320), wh = S(800);
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) { wa.left = wa.top = 0; wa.right = 1024; wa.bottom = 740; }
        if (ww > wa.right - wa.left) ww = wa.right - wa.left;
        if (wh > wa.bottom - wa.top) wh = wa.bottom - wa.top;
        g_narrow = wa.right - wa.left < S(1200);
        g_main = CreateWindowExA(0, "KitStudioMain", APP_NAME, WS_OVERLAPPEDWINDOW,
                                 wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2,
                                 ww, wh, NULL, make_menu(), inst, NULL);
    }
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
    if (g_narrow) sim_float(1);         /* small screen: the simulator in its own window */
    SetFocus(g_edit);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (TranslateAcceleratorA(g_main, g_accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
