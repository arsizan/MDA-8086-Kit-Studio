/* kitcore.c - talk to the MDA-8086 serial monitor.
 *
 * Protocol (MDA-Win8086 manual, ch. 4, and checked on the lab's kit):
 *   kit in PC mode + RESET  -> "** 8086 Monitor 9.5 **" ... "8086 >"
 *   "\r"                    -> new "8086 >" prompt
 *   "L\r"                   -> "Down load start !!", then Intel HEX records,
 *                              after the end record: "OK Completed !!" + prompt
 *   "G 0000:1000\r"         -> "Execute Address = 0000:1000", program runs
 * Characters are sent with a small gap (2 ms) so the kit never misses one.
 * The words above are searched only in the visible characters the kit sent: an invisible
 * byte such as NUL must not hide them (1.1 said "did not confirm the start" on the lab's
 * kit although "Execute Address" was on the screen).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "kitcore.h"

#define CHAR_GAP_US 2000

typedef struct {
    char txt[512];
    int  len;
} Acc;

/* ---- raw serial log (for finding out what a real kit sends) ---- */
#define LOG_LIMIT 2000000L

void kit_set_log(KitLink *k, FILE *f)
{
    k->log = f;
    k->log_t0 = kp_ms();
    k->log_size = 0;
}

static void log_line(KitLink *k, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (!k->log) return;
    if (k->log_size > LOG_LIMIT) {
        fprintf(k->log, "(log limit reached, nothing more is written in this session)\n");
        fflush(k->log);
        k->log = NULL;
        return;
    }
    n = fprintf(k->log, "[%9.3f] ", (unsigned)(kp_ms() - k->log_t0) / 1000.0);
    va_start(ap, fmt);
    n += vfprintf(k->log, fmt, ap);
    va_end(ap);
    fputc('\n', k->log);
    fflush(k->log);
    if (n > 0) k->log_size += n + 1;
}

/* bytes as hex, 16 per line, with the printable characters beside them */
static void log_bytes(KitLink *k, const char *dir, const char *s, int n)
{
    int i, j;
    if (!k->log) return;
    for (i = 0; i < n; i += 16) {
        char hex[16 * 3 + 1], asc[17];
        int m = n - i < 16 ? n - i : 16;
        for (j = 0; j < m; j++) {
            unsigned char c = (unsigned char)s[i + j];
            snprintf(hex + j * 3, 4, "%02X ", c);
            asc[j] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        hex[m * 3] = 0;
        asc[m] = 0;
        log_line(k, "%s %-48s %s", dir, hex, asc);
    }
}

static void say(KitLink *k, int kind, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (kind != KO_KIT) log_line(k, "-- %s", buf);
    if (k->out) k->out(k->ctx, kind, buf);
}

static int cancelled(KitLink *k) { return k->cancel && *k->cancel; }

static int kit_strnicmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        int ca = tolower((unsigned char)*a++), cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

static const char *stristr(const char *h, const char *n)
{
    size_t ln = strlen(n);
    for (; *h; h++)
        if (kit_strnicmp(h, n, ln) == 0) return h;
    return NULL;
}

/* remember kit text for the word searches. Only what the output pane can show is kept
   (printable characters, CR, LF, TAB): a NUL or other control byte is invisible on the
   screen, but in the text it would end the C string and hide every word after it. */
static void acc_add(Acc *a, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c != '\r' && c != '\n' && c != '\t' && (c < 32 || c > 126)) continue;
        if (a->len >= (int)sizeof a->txt - 1) {         /* full: keep the newest half */
            int keep = (int)sizeof a->txt / 2;
            memmove(a->txt, a->txt + a->len - keep, keep);
            a->len = keep;
        }
        a->txt[a->len++] = (char)c;
    }
    a->txt[a->len] = 0;
}

/* clean kit text for display: CR / CRLF -> '\n', drop NUL, BEL and other controls */
static int clean_text(KitLink *k, const char *in, int n, char *out)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r') { out[m++] = '\n'; k->cr_pending = 1; continue; }
        if (c == '\n') {
            if (!k->cr_pending) out[m++] = '\n';
            k->cr_pending = 0;
            continue;
        }
        k->cr_pending = 0;
        if (c == '\t' || (c >= 32 && c < 127)) out[m++] = (char)c;
    }
    out[m] = 0;
    return m;
}

/* read what is waiting on port p; show it (if show) and remember it in acc */
static int pump(KitLink *k, KitPort *p, Acc *acc, int show)
{
    char raw[512], txt[1100];
    int total = 0;
    for (;;) {
        int n = kp_read(p, raw, sizeof raw);
        if (n < 0) { log_line(k, "RX failed (port stopped working)"); return -1; }
        if (n == 0) break;
        log_bytes(k, "RX", raw, n);
        total += n;
        if (acc) acc_add(acc, raw, n);
        if (show) {
            int m = clean_text(k, raw, n, txt);
            if (m) say(k, KO_KIT, "%s", txt);
        }
    }
    return total;
}

/* wait until one of the words appears; returns its index, -1 timeout, -2 cancelled, -3 port failed */
static int wait_for(KitLink *k, KitPort *p, Acc *acc, const char *const *words, int nwords,
                    int timeout_ms, int show)
{
    unsigned t0 = kp_ms();
    for (;;) {
        int i;
        if (pump(k, p, acc, show) < 0) return -3;
        for (i = 0; i < nwords; i++)
            if (stristr(acc->txt, words[i])) return i;
        if (cancelled(k)) return -2;
        if ((int)(kp_ms() - t0) >= timeout_ms) return -1;
        kp_sleep(15);
    }
}

static int send_slow(KitLink *k, KitPort *p, const char *s, Acc *acc, int show)
{
    int i = 0;
    log_bytes(k, "TX", s, (int)strlen(s));
    for (; s[i]; i++) {
        if (kp_write(p, s + i, 1) != 1) return -1;
        kp_wait_us(CHAR_GAP_US);
        if ((i & 15) == 15 && pump(k, p, acc, show) < 0) return -1;
    }
    return pump(k, p, acc, show) < 0 ? -1 : 0;
}

static const char *const PROMPT_WORDS[] = { "8086", ">" };

/* does the kit on p answer to Enter with its prompt? */
static int test_port(KitLink *k, KitPort *p)
{
    Acc acc;
    acc.len = 0; acc.txt[0] = 0;
    kp_flush_input(p);
    log_line(k, "test %s: input flushed", kp_name(p));
    log_bytes(k, "TX", "\r", 1);
    if (kp_write(p, "\r", 1) != 1) return 0;
    return wait_for(k, p, &acc, PROMPT_WORDS, 2, 800, 0) >= 0;
}

void kit_init(KitLink *k, KitOutFn out, void *ctx, volatile long *cancel)
{
    memset(k, 0, sizeof *k);
    k->out = out;
    k->ctx = ctx;
    k->cancel = cancel;
    k->wait_reset_ms = 60000;
}

void kit_disconnect(KitLink *k)
{
    if (k->port) {
        kp_close(k->port);
        k->port = NULL;
    }
}

int kit_find(KitLink *k, int allow_wait)
{
    char names[KP_MAXPORTS][KP_NAMELEN];
    KitPort *open[KP_MAXPORTS];
    Acc acc[KP_MAXPORTS];
    int nnames = 0, nopen = 0, i, found = -1;
    char busy[600] = "";

    /* already connected and answering? */
    if (k->port) {
        if (test_port(k, k->port)) return 1;
        open[nopen++] = k->port;          /* keep it: we may wait for RESET on it */
        k->port = NULL;
    }
    if (k->want[0]) {
        snprintf(names[0], KP_NAMELEN, "%s", k->want);
        nnames = 1;
    } else {
        nnames = kp_list(names, KP_MAXPORTS);
    }
    for (i = 0; i < nnames && nopen < KP_MAXPORTS; i++) {
        char err[200];
        KitPort *p;
        if (nopen > 0 && strcmp(kp_name(open[0]), names[i]) == 0) continue;
        p = kp_open(names[i], err, sizeof err);
        if (p) open[nopen++] = p;
        else if (strlen(busy) + strlen(err) + 4 < sizeof busy) {
            strcat(busy, "\n  ");
            strcat(busy, err);
        }
    }
    if (nopen == 0) {
        if (nnames == 0)
            say(k, KO_ERR, "No COM port found on this PC.\n"
                "Connect the kit's 9-pin cable to a COM port, or use a USB-to-RS232 adapter.");
        else
            say(k, KO_ERR, "No COM port could be opened:%s", busy);
        return 0;
    }
    {
        char list[600] = "";
        for (i = 0; i < nopen; i++) {
            if (i) strcat(list, ", ");
            strcat(list, kp_name(open[i]));
        }
        say(k, KO_INFO, "Looking for the kit on %s ...", list);
    }
    if (busy[0]) say(k, KO_WARN, "Skipped:%s", busy);

    for (i = 0; i < nopen && found < 0; i++)
        if (test_port(k, open[i])) found = i;

    if (found < 0 && allow_wait) {
        unsigned t0 = kp_ms(), poke = t0;
        int wait_ms = k->wait_reset_ms > 0 ? k->wait_reset_ms : 60000;
        say(k, KO_WARN, "The kit is not answering.\n"
            "On the kit: put the SELECT MODE switch to PC, then press RESET.\n"
            "Waiting up to %d seconds (press Stop to cancel) ...", wait_ms / 1000);
        for (i = 0; i < nopen; i++) { acc[i].len = 0; acc[i].txt[0] = 0; }
        while (found < 0) {
            for (i = 0; i < nopen && found < 0; i++) {
                if (!open[i]) continue;
                if (pump(k, open[i], &acc[i], 0) < 0) {
                    say(k, KO_WARN, "%s stopped working (cable or adapter unplugged?)", kp_name(open[i]));
                    kp_close(open[i]);
                    open[i] = NULL;
                    continue;
                }
                if (stristr(acc[i].txt, "8086") || stristr(acc[i].txt, ">")) found = i;
            }
            if (found >= 0) break;
            if (cancelled(k)) { say(k, KO_WARN, "Stopped."); break; }
            if ((int)(kp_ms() - t0) >= wait_ms) break;
            if ((int)(kp_ms() - poke) >= 2000) {
                for (i = 0; i < nopen; i++)
                    if (open[i]) {
                        log_line(k, "TX 0D (Enter) on %s", kp_name(open[i]));
                        kp_write(open[i], "\r", 1);
                    }
                poke = kp_ms();
            }
            kp_sleep(50);
        }
        if (found >= 0) {
            char txt[1100];
            int m = clean_text(k, acc[found].txt, acc[found].len, txt);
            if (m) say(k, KO_KIT, "%s", txt);
            kp_sleep(300);                 /* let the banner finish */
            pump(k, open[found], NULL, 1);
        }
    }
    for (i = 0; i < nopen; i++)
        if (open[i] && i != found) kp_close(open[i]);
    if (found < 0) {
        if (!cancelled(k))
            say(k, KO_ERR, "Kit not found. Check:\n"
                "  - kit power is ON and SELECT MODE switch is on PC\n"
                "  - the 9-pin serial cable is in the PC's COM port (or USB-RS232 adapter)\n"
                "  - PuTTY or another program is not using the port\n"
                "  - press RESET on the kit while this program is waiting");
        return 0;
    }
    k->port = open[found];
    k->cr_pending = 0;
    say(k, KO_OK, "Kit found on %s (9600 baud).", kp_name(k->port));
    return 1;
}

int kit_check_abs(const char *abs, int *nbytes, char *why, int whysize)
{
    const char *p = abs;
    int recs = 0, line = 0;
    *nbytes = 0;
    while (*p) {
        const char *e = p;
        line++;
        while (*e && *e != '\n') e++;
        while (p < e && (*p == ' ' || *p == '\t' || *p == 26)) p++;
        if (p < e && *p == ':') {
            int n = 0, sum = 0;
            unsigned char b[300];
            const char *q = p + 1;
            while (q + 1 < e && isxdigit((unsigned char)q[0]) && isxdigit((unsigned char)q[1]) && n < 300) {
                char h[3] = { q[0], q[1], 0 };
                b[n] = (unsigned char)strtol(h, NULL, 16);
                sum += b[n++];
                q += 2;
            }
            if (n < 5 || n != b[0] + 5 || (sum & 0xFF)) {
                snprintf(why, whysize, "Line %d of the program file is damaged.", line);
                return -1;
            }
            if (b[3] == 0) { recs++; *nbytes += b[0]; }
            if (b[3] == 1) break;
        }
        p = *e ? e + 1 : e;
    }
    return recs;
}

int kit_download(KitLink *k, const char *abs, int *nbytes)
{
    static const char *const START[] = { "start" };
    static const char *const DONE[] = { "Completed", "rror" };
    Acc acc;
    const char *p = abs;
    char line[600];
    int recs, r, sent_end = 0, bytes = 0;
    char why[200];

    recs = kit_check_abs(abs, &bytes, why, sizeof why);
    if (nbytes) *nbytes = bytes;
    if (recs < 0) { say(k, KO_ERR, "%s", why); return 0; }
    if (recs == 0) { say(k, KO_ERR, "The program has no bytes to send."); return 0; }
    if (!k->port) { say(k, KO_ERR, "Kit is not connected."); return 0; }

    say(k, KO_INFO, "Sending %d bytes to the kit (L command) ...", bytes);
    acc.len = 0; acc.txt[0] = 0;
    kp_flush_input(k->port);
    log_line(k, "input flushed");
    k->cr_pending = 0;
    if (send_slow(k, k->port, "L\r", &acc, 1) < 0) goto port_failed;
    r = wait_for(k, k->port, &acc, START, 1, 2000, 1);
    if (r == -3) goto port_failed;
    if (r == -2) { say(k, KO_WARN, "Stopped."); return 0; }
    if (r < 0) {
        say(k, KO_ERR, "The kit did not start the download (no \"Down load start\").\n"
            "Press RESET on the kit and try again.");
        return 0;
    }
    while (*p) {
        const char *e = p;
        int n;
        while (*e && *e != '\n') e++;
        while (p < e && (*p == ' ' || *p == '\t' || *p == 26)) p++;
        n = (int)(e - p);
        while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == 26)) n--;
        if (n > 0 && *p == ':' && n < (int)sizeof line - 3) {
            memcpy(line, p, n);
            strcpy(line + n, "\r\n");
            if (send_slow(k, k->port, line, &acc, 1) < 0) goto port_failed;
            if (n >= 9 && line[7] == '0' && line[8] == '1') { sent_end = 1; break; }
            if (cancelled(k)) { say(k, KO_WARN, "Stopped while sending."); return 0; }
        }
        p = *e ? e + 1 : e;
    }
    if (!sent_end && send_slow(k, k->port, ":00000001FF\r\n", &acc, 1) < 0) goto port_failed;
    r = wait_for(k, k->port, &acc, DONE, 2, 4000, 1);
    if (r == -3) goto port_failed;
    if (r == 0) {
        static const char *const PR[] = { "8086 >", "8086>" };
        wait_for(k, k->port, &acc, PR, 2, 500, 1);
        say(k, KO_OK, "Download complete.");
        return 1;
    }
    if (r == 1) say(k, KO_ERR, "The kit reported an error while loading the program.");
    else if (r == -2) say(k, KO_WARN, "Stopped.");
    else say(k, KO_ERR, "The kit did not confirm the download (no \"OK Completed\").");
    return 0;

port_failed:
    say(k, KO_ERR, "The COM port stopped working (cable or USB adapter unplugged?).");
    kit_disconnect(k);
    return 0;
}

int kit_go(KitLink *k, unsigned seg, unsigned off)
{
    static const char *const EXE[] = { "Execute" };
    Acc acc;
    char cmd[40];
    int r;
    if (!k->port) { say(k, KO_ERR, "Kit is not connected."); return 0; }
    acc.len = 0; acc.txt[0] = 0;
    snprintf(cmd, sizeof cmd, "G %04X:%04X\r", seg & 0xFFFF, off & 0xFFFF);
    k->cr_pending = 0;
    if (send_slow(k, k->port, cmd, &acc, 1) < 0) {
        say(k, KO_ERR, "The COM port stopped working.");
        kit_disconnect(k);
        return 0;
    }
    r = wait_for(k, k->port, &acc, EXE, 1, 2000, 1);
    if (r == 0) {
        kp_sleep(100);
        pump(k, k->port, NULL, 1);
        say(k, KO_OK, "Program is running on the kit. Press RESET on the kit to stop it.");
        return 1;
    }
    if (r == -3) { say(k, KO_ERR, "The COM port stopped working."); kit_disconnect(k); return 0; }
    if (r == -2) { say(k, KO_WARN, "Stopped."); return 0; }
    say(k, KO_WARN, "The kit did not answer \"Execute Address\" to the G command.\n"
        "If the program is not running, press RESET on the kit and press Run again.");
    return 0;
}

int kit_command(KitLink *k, const char *line)
{
    char buf[300];
    if (!k->port) { say(k, KO_ERR, "Kit is not connected."); return 0; }
    snprintf(buf, sizeof buf, "%s\r", line);
    k->cr_pending = 0;
    if (send_slow(k, k->port, buf, NULL, 1) < 0) {
        say(k, KO_ERR, "The COM port stopped working.");
        kit_disconnect(k);
        return 0;
    }
    return 1;
}

int kit_poll(KitLink *k)
{
    if (!k->port) return 0;
    if (pump(k, k->port, NULL, 1) < 0) {
        say(k, KO_WARN, "%s stopped working (cable or adapter unplugged?)", kp_name(k->port));
        kit_disconnect(k);
        return -1;
    }
    return 0;
}
