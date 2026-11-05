/* kitview.c - the Simulator panel's picture (see kitview.h). Designed for 580 x 640 and
 * scaled to the panel's size. Light colours, every LED drawn with a clear outline. */
#include <stdio.h>
#include <string.h>
#include "kitview.h"

#define DW 580.0
#define DH 640.0

/* colours */
#define C_BG       0xF2F4F7
#define C_CARD     0xFFFFFF
#define C_CARDLINE 0xD6DBE1
#define C_TITLE    0x1F2D3A
#define C_SUB      0x6A7684
#define C_FAINT    0xA3ACB7
#define C_TEXT     0x1F2D3A
#define C_OFF      0xE4E8EC     /* LED / segment off */
#define C_OFFLINE  0x98A3AF
#define C_RED      0xE53935
#define C_REDLINE  0xB3261E
#define C_GREEN    0x22B14C
#define C_GREENLINE 0x147A32
#define C_YELLOW   0xFFC107
#define C_YELLOWLINE 0xC48F00
#define C_BOARD    0xF7F8FA

typedef struct { const KitDraw *d; double s, ox, oy; } V;

static void R(V *v, double x, double y, double w, double h, double r, unsigned fill, unsigned line)
{ v->d->rect(v->d->ctx, v->ox + x * v->s, v->oy + y * v->s, w * v->s, h * v->s, r * v->s, fill, line); }
static void C(V *v, double cx, double cy, double r, unsigned fill, unsigned line, double lw)
{ v->d->circle(v->d->ctx, v->ox + cx * v->s, v->oy + cy * v->s, r * v->s, fill, line, lw * v->s); }
static void T(V *v, double x, double y, const char *s, double size, int flags, unsigned color)
{ v->d->text(v->d->ctx, v->ox + x * v->s, v->oy + y * v->s, s, size * v->s, flags, color); }
static void P(V *v, const double *xy, int n, unsigned fill)
{
    double t[16];
    int i;
    for (i = 0; i < n && i < 8; i++) {
        t[2 * i] = v->ox + xy[2 * i] * v->s;
        t[2 * i + 1] = v->oy + xy[2 * i + 1] * v->s;
    }
    v->d->poly(v->d->ctx, t, n < 8 ? n : 8, fill);
}

/* colour between a and b (k = 0..255) */
static unsigned mix(unsigned a, unsigned b, int k)
{
    unsigned out = 0;
    int sh;
    for (sh = 0; sh <= 16; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= (unsigned)(ca + (cb - ca) * k / 255) << sh;
    }
    return out;
}

static void card(V *v, double x, double y, double w, double h, const char *title, const char *sub)
{
    R(v, x, y, w, h, 8, C_CARD, C_CARDLINE);
    T(v, x + 12, y + 9, title, 12.5, KV_BOLD, C_TITLE);
    if (sub) T(v, x + 12, y + 27, sub, 10.5, 0, C_SUB);
}

/* one LED: brightness k of colour (on, online) */
static void led(V *v, double cx, double cy, double r, int k, unsigned on, unsigned online)
{
    C(v, cx, cy, r, mix(C_OFF, on, k), mix(C_OFFLINE, online, k), 1.4);
    if (k > 40) C(v, cx - r * 0.32, cy - r * 0.32, r * 0.28, mix(mix(C_OFF, on, k), 0xFFFFFF, 110), KV_NONE, 0);
}

static void bin8(unsigned v, char *out)
{
    int i;
    for (i = 0; i < 8; i++) out[i] = (char)('0' + ((v >> (7 - i)) & 1));
    out[8] = 0;
}

void kitview_speed_text(double speed, char *buf, int size)
{
    if (speed >= 0.999) snprintf(buf, size, "1x (real kit)");
    else snprintf(buf, size, "1/%.0fx (slow motion)", 1.0 / speed);
}

/* ------------------------------------------------------------------ parts */
static void status_strip(V *v, const KitPanel *p)
{
    const KitView *k = &p->v;
    const char *pill;
    unsigned pc;
    char buf[200], b2[64];
    R(v, 8, 8, 564, 36, 8, C_CARD, C_CARDLINE);
    if (!p->have) { pill = "NO PROGRAM"; pc = 0x8A96A3; }
    else if (k->stop >= 0) { pill = "STOPPED"; pc = 0xD93025; }
    else if (p->running) { pill = "RUNNING"; pc = 0x1E9E4A; }
    else if (k->insns == 0) { pill = "READY"; pc = 0x1E6FD9; }
    else { pill = "PAUSED"; pc = 0xE08A00; }
    R(v, 14, 14, 96, 24, 12, pc, KV_NONE);
    T(v, 62, 19, pill, 11, KV_BOLD | KV_CENTER, 0xFFFFFF);
    if (!p->have) {
        T(v, 120, 19, "Run (F6) or Step (F8): the program in the editor is loaded here.", 11.5, 0, C_SUB);
        return;
    }
    if (k->stop >= 0) {
        switch (k->stop) {
        case SIM_INT3:    snprintf(buf, sizeof buf, "INT 3: back to the kit's monitor."); break;
        case SIM_HLT:     snprintf(buf, sizeof buf, "HLT: the CPU stopped."); break;
        case SIM_DOS:     snprintf(buf, sizeof buf, "INT 21H is DOS - the kit has no DOS."); break;
        case SIM_INT:     snprintf(buf, sizeof buf, "INT %02XH: the kit has no handler for it.", k->stop_int & 0xFF); break;
        case SIM_BADOP:   snprintf(buf, sizeof buf, "Byte %02XH is not an 8086 instruction.", k->stop_op & 0xFF); break;
        case SIM_NOT8086: snprintf(buf, sizeof buf, "Instruction %02XH is not on the 8086 (80186+).", k->stop_op & 0xFF); break;
        case SIM_OUTSIDE: snprintf(buf, sizeof buf, "Ran past the end of the program (end with a JMP loop)."); break;
        case SIM_ROM:     snprintf(buf, sizeof buf, "Jumped into the monitor ROM."); break;
        case SIM_DIV0:    snprintf(buf, sizeof buf, "Divide error (divide by 0 or result too big)."); break;
        default:          snprintf(buf, sizeof buf, "Stopped."); break;
        }
        T(v, 120, 19, buf, 11.5, 0, 0xB3261E);
    } else if (p->running) {
        kitview_speed_text(p->speed, b2, sizeof b2);
        snprintf(buf, sizeof buf, "%s   %.2f s of kit time   %lu instructions", b2,
                 (double)k->cycles / SIM_CLOCK, k->insns);
        T(v, 120, 19, buf, 11.5, 0, C_TEXT);
    } else if (p->line > 0) {
        snprintf(buf, sizeof buf, "Next: line %d", p->line);
        T(v, 120, 19, buf, 11.5, KV_BOLD, C_TEXT);
        T(v, 214, 19, p->line_text, 11.5, KV_MONO, C_TEXT);
    } else {
        T(v, 120, 19, "Next instruction:", 11.5, KV_BOLD, C_TEXT);
    }
    snprintf(buf, sizeof buf, "CS:IP %04X:%04X", k->cs, k->ip);
    if (!p->running || k->stop >= 0) T(v, 566, 21, buf, 10.5, KV_MONO | KV_RIGHT, C_SUB);
}

static void dot_matrix(V *v, const KitView *k)
{
    const double px = 26, r = 10, bx = 78, by = 104;      /* pitch, LED radius, board corner */
    char s[8];
    int x, y;
    card(v, 8, 52, 330, 300, "DOT MATRIX 8 x 8", "8255 #1:  red 18H   green 1AH   column 1CH");
    R(v, bx, by, 8 * px + 12, 8 * px + 12, 8, C_BOARD, C_CARDLINE);
    for (x = 0; x < 8; x++) {               /* column bits: bit 0 = left */
        snprintf(s, sizeof s, "%d", x);
        T(v, bx + 6 + px * x + px / 2, by - 14, s, 10, KV_CENTER, C_FAINT);
    }
    for (y = 0; y < 8; y++) {               /* row bits: bit 7 = top */
        snprintf(s, sizeof s, "%d", 7 - y);
        T(v, bx - 9, by + 6 + px * y + px / 2 - 6, s, 10, KV_CENTER, C_FAINT);
        for (x = 0; x < 8; x++) {
            int g = k->dm_green[y * 8 + x], rd = k->dm_red[y * 8 + x];
            double cx = bx + 6 + px * x + px / 2, cy = by + 6 + px * y + px / 2;
            if (g && rd) {                  /* both = yellow, as bright as the dimmer one */
                int b = g < rd ? g : rd;
                led(v, cx, cy, r, b, C_YELLOW, C_YELLOWLINE);
            } else if (rd) led(v, cx, cy, r, rd, C_RED, C_REDLINE);
            else led(v, cx, cy, r, g, C_GREEN, C_GREENLINE);
        }
    }
    T(v, 20, 326, "Rows: bit 7 = top row, 0 = LED on (18H red, 1AH green)", 10, 0, C_SUB);
    T(v, 20, 339, "Columns (1CH): bit 0 = left column, 1 = on", 10, 0, C_SUB);
}

static void hseg(V *v, double x1, double x2, double y, double t, unsigned c)
{
    double p[12] = { x1, y, x1 + t / 2, y - t / 2, x2 - t / 2, y - t / 2, x2, y, x2 - t / 2, y + t / 2, x1 + t / 2, y + t / 2 };
    P(v, p, 6, c);
}

static void vseg(V *v, double x, double y1, double y2, double t, unsigned c)
{
    double p[12] = { x, y1, x + t / 2, y1 + t / 2, x + t / 2, y2 - t / 2, x, y2, x - t / 2, y2 - t / 2, x - t / 2, y1 + t / 2 };
    P(v, p, 6, c);
}

static void seven_seg(V *v, const KitView *k)
{
    static const unsigned ledc[4] = { C_RED, C_GREEN, C_YELLOW, C_RED };
    static const unsigned ledl[4] = { C_REDLINE, C_GREENLINE, C_YELLOWLINE, C_REDLINE };
    static const char *const ledn[4] = { "red", "green", "yellow", "red" };
    const double gx = 419, gy = 182, W = 76, M = 66, B = 132, t = 13, g = 2.5;
    unsigned c[8];
    char buf[40];
    int i;
    card(v, 346, 52, 226, 300, "LEDs & 7-SEGMENT", "8255 #2:  PB 1BH = LEDs (1 = on)");
    for (i = 0; i < 4; i++) {
        double cx = 383 + i * 51;
        char nm[8];
        led(v, cx, 102, 11, k->led[i], ledc[i], ledl[i]);
        snprintf(nm, sizeof nm, "PB%d", i);
        T(v, cx, 116, nm, 9.5, KV_CENTER | KV_BOLD, C_SUB);
        T(v, cx, 128, ledn[i], 9, KV_CENTER, C_FAINT);
    }
    T(v, 358, 143, "PA 19H = segments (0 = on)", 10.5, 0, C_SUB);
    R(v, 384, 160, 150, 168, 8, C_BOARD, C_CARDLINE);
    for (i = 0; i < 8; i++) c[i] = mix(C_OFF, C_RED, k->seg[i]);
    hseg(v, gx + g, gx + W - g, gy, t, c[0]);              /* a */
    vseg(v, gx + W, gy + g, gy + M - g, t, c[1]);          /* b */
    vseg(v, gx + W, gy + M + g, gy + B - g, t, c[2]);      /* c */
    hseg(v, gx + g, gx + W - g, gy + B, t, c[3]);          /* d */
    vseg(v, gx, gy + M + g, gy + B - g, t, c[4]);          /* e */
    vseg(v, gx, gy + g, gy + M - g, t, c[5]);              /* f */
    hseg(v, gx + g, gx + W - g, gy + M, t, c[6]);          /* g */
    C(v, gx + W + 15, gy + B, 6.5, c[7], KV_NONE, 0);       /* dp */
    T(v, gx + W / 2, gy - 21, "a", 9.5, KV_CENTER, C_FAINT);
    T(v, gx + W + 14, gy + M / 2 - 6, "b", 9.5, KV_CENTER, C_FAINT);
    T(v, gx + W + 14, gy + M + (B - M) / 2 - 6, "c", 9.5, KV_CENTER, C_FAINT);
    T(v, gx + W / 2, gy + B + 8, "d", 9.5, KV_CENTER, C_FAINT);
    T(v, gx - 14, gy + M + (B - M) / 2 - 6, "e", 9.5, KV_CENTER, C_FAINT);
    T(v, gx - 14, gy + M / 2 - 6, "f", 9.5, KV_CENTER, C_FAINT);
    T(v, gx + W / 2, gy + M - 22, "g", 9.5, KV_CENTER, C_FAINT);
    snprintf(buf, sizeof buf, "19H = %02XH   bit 0 = a ... bit 7 = dp", k->p2[0]);
    T(v, 459, 334, buf, 10, KV_CENTER, C_SUB);
}

static void lcd(V *v, const KitView *k)
{
    unsigned glass = k->lcd_on ? 0xB9D88C : 0xCBD0C6, cell = k->lcd_on ? 0xAACB7B : 0xC2C7BC;
    int r, i;
    card(v, 8, 360, 564, 88, "LCD 16 x 2", NULL);
    T(v, 20, 386, "00H command", 10, 0, C_SUB);
    T(v, 20, 400, "04H data", 10, 0, C_SUB);
    T(v, 20, 414, "02H status", 10, 0, C_SUB);
    R(v, 136, 368, 428, 72, 6, glass, 0x6F8A4F);
    for (r = 0; r < 2; r++)
        for (i = 0; i < 16; i++) {
            char ch[2];
            double x = 146 + i * 25.5, y = 375 + r * 31;
            R(v, x, y, 23, 28, 2, cell, KV_NONE);
            ch[0] = k->lcd[r][i];
            ch[1] = 0;
            if (k->lcd_on && ch[0] != ' ') T(v, x + 11.5, y + 4, ch, 18, KV_MONO | KV_BOLD | KV_CENTER, 0x1D3010);
        }
}

static void registers(V *v, const KitView *k)
{
    static const char *const fl = "ODITSZAPC";
    static const unsigned fbit[9] = { 0x800, 0x400, 0x200, 0x100, 0x80, 0x40, 0x10, 0x04, 0x01 };
    const char *nm[13] = { "AX", "BX", "CX", "DX", "SI", "DI", "BP", "SP", "CS", "DS", "ES", "SS", "IP" };
    unsigned val[13];
    char buf[80];
    int i;
    val[0] = k->ax; val[1] = k->bx; val[2] = k->cx; val[3] = k->dx;
    val[4] = k->si; val[5] = k->di; val[6] = k->bp; val[7] = k->sp;
    val[8] = k->cs; val[9] = k->ds; val[10] = k->es; val[11] = k->ss; val[12] = k->ip;
    card(v, 8, 456, 330, 176, "REGISTERS", NULL);
    for (i = 0; i < 13; i++) {
        double x = 20 + (i % 4) * 78, y = 482 + (i / 4) * 21;
        T(v, x, y, nm[i], 12, KV_MONO, C_SUB);
        snprintf(buf, sizeof buf, "%04X", val[i]);
        T(v, x + 24, y, buf, 12, KV_MONO | KV_BOLD, C_TEXT);
    }
    T(v, 98, 545, "FLAGS", 10, KV_BOLD, C_SUB);
    for (i = 0; i < 9; i++) {
        int on = (k->flags & fbit[i]) != 0;
        char ch[2];
        double x = 140 + i * 21;
        ch[0] = fl[i];
        ch[1] = 0;
        R(v, x, 541, 19, 19, 4, on ? 0x1E6FD9 : 0xEDF0F3, on ? KV_NONE : 0xD6DBE1);
        T(v, x + 9.5, 544, ch, 11, KV_BOLD | KV_CENTER, on ? 0xFFFFFF : 0x9AA5B1);
    }
    snprintf(buf, sizeof buf, "kit time %.3f s   %lu instructions", (double)k->cycles / SIM_CLOCK, k->insns);
    T(v, 20, 575, buf, 10, 0, C_SUB);
    T(v, 20, 595, "Flags: O overflow, D direction, I interrupt, T trap,", 9.5, 0, C_FAINT);
    T(v, 20, 609, "S sign, Z zero, A aux carry, P parity, C carry", 9.5, 0, C_FAINT);
}

static void ports(V *v, const KitView *k)
{
    static const char *const pn[4] = { "A ", "B ", "C ", "CR" };
    char buf[64], b[9];
    int chip, i;
    card(v, 346, 456, 226, 176, "8255 PORTS", NULL);
    for (chip = 0; chip < 2; chip++) {
        const unsigned char *lat = chip ? k->p2 : k->p1;
        unsigned cr = chip ? k->p2cr : k->p1cr;
        double y0 = chip ? 556 : 478;
        T(v, 358, y0, chip ? "#2  LEDs, 7-segment" : "#1  dot matrix", 10.5, KV_BOLD, C_TITLE);
        for (i = 0; i < 4; i++) {
            unsigned port = 0x18 + chip + i * 2, val = i < 3 ? lat[i] : cr;
            if (i == 3 && !cr) snprintf(buf, sizeof buf, "%02XH %s  --  not set up", port, pn[i]);
            else { bin8(val, b); snprintf(buf, sizeof buf, "%02XH %s  %02X  %s", port, pn[i], val, b); }
            T(v, 358, y0 + 16 + i * 14, buf, 10.5, KV_MONO, i == 3 && !cr ? 0xB3261E : C_TEXT);
        }
    }
}

/* ------------------------------------------------------------------ all */
void kitview_draw(const KitDraw *d, double w, double h, const KitPanel *p)
{
    V v;
    double sx = w / DW, sy = h / DH;
    v.d = d;
    v.s = sx < sy ? sx : sy;
    v.ox = (w - DW * v.s) / 2;
    v.oy = 0;
    d->rect(d->ctx, 0, 0, w, h, 0, C_BG, KV_NONE);
    status_strip(&v, p);
    dot_matrix(&v, &p->v);
    seven_seg(&v, &p->v);
    lcd(&v, &p->v);
    registers(&v, &p->v);
    ports(&v, &p->v);
}
