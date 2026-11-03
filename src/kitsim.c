/* kitsim.c - a model of the MDA-8086 kit for checking programs without the kit.
 *
 * CPU: the 8086 instruction set (no 80186+ instructions, no 8087), with clock counts
 * from the 8086 tables, so delay loops take about as long as on the kit
 * (4.9152 MHz = 14.7456 MHz crystal / 3).
 * Start state as after RESET on the kit (manual, REG key): CS=DS=ES=SS=0000,
 * SP=0540H, IP=1000H, all other registers 0.
 *
 * Devices (manual, I/O address map):
 *   8255 CS1  18H port A = red rows, 1AH port B = green rows, 1CH port C = columns,
 *             1EH control.  Rows: bit 7 = top row, 0 = LED on.  Columns: bit 0 = left,
 *             1 = column on.
 *   8255 CS2  19H port A = 7-segment (bit0 a ... bit6 g, bit7 dp, 0 = segment on),
 *             1BH port B (LEDs, stepping motor), 1DH port C, 1FH control.
 *   Until a mode word (bit 7 = 1) is written, a 8255 drives nothing (all ports input).
 *   A mode word clears the output latches, as on the real chip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "kitsim.h"

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

#define WIN_SEC      0.05         /* one picture = 0.05 s of LED light */
#define MAX_WINDOWS  400
#define DIGIT_MIN    0.02         /* a 7-segment value must stay 20 ms to count */

enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS };

typedef struct {
    int set;                      /* got a mode word */
    unsigned char latch[3];       /* A, B, C output latches */
    int in_a, in_b, in_cl, in_cu; /* port (half) is an input */
} Ppi;

typedef struct {
    unsigned short r[8], s[4], ip, fl;
    unsigned short ip0, cs0;      /* start of the current instruction */
    unsigned char *m;             /* 1 MB */
    unsigned char vec[256];       /* the program wrote this interrupt vector */
    unsigned long long cyc;
    unsigned long insns;
    unsigned lo, hi;              /* the program's bytes */
    int stop;                     /* -1 = running */
    int seg_ovr, repx;           /* segment override, REP prefix (F2/F3) */
    int mod, reg, rm, ea_seg;
    unsigned ea_off, ea_cyc;

    Ppi p1, p2;
    /* dot matrix */
    unsigned long long win_start, win_len, last;
    unsigned long long on_g[64], on_r[64];
    int nwin;
    unsigned long long wg[MAX_WINDOWS], wr[MAX_WINDOWS];   /* lit LEDs per window (bit = row*8+col) */
    double wt[MAX_WINDOWS];
    int started;
    /* 7-segment */
    int seg_cur, seg_have;
    unsigned long long seg_since;

    KitSimReport *rep;
} Sim;

static unsigned char parity[256];

/* ------------------------------------------------------------------ memory */
static unsigned phys(unsigned seg, unsigned off) { return ((seg << 4) + (off & 0xFFFF)) & 0xFFFFF; }
static unsigned rb(Sim *c, unsigned seg, unsigned off) { return c->m[phys(seg, off)]; }
static unsigned rw(Sim *c, unsigned seg, unsigned off) { return rb(c, seg, off) | (rb(c, seg, off + 1) << 8); }
static void wb(Sim *c, unsigned seg, unsigned off, unsigned v)
{
    unsigned a = phys(seg, off);
    if (a >= 0xF0000) return;               /* monitor ROM */
    if (a < 0x400) c->vec[a >> 2] = 1;
    c->m[a] = (unsigned char)v;
}
static void ww(Sim *c, unsigned seg, unsigned off, unsigned v)
{
    wb(c, seg, off, v & 0xFF);
    wb(c, seg, off + 1, (v >> 8) & 0xFF);
}
static unsigned fetch8(Sim *c) { unsigned v = rb(c, c->s[S_CS], c->ip); c->ip++; return v; }
static unsigned fetch16(Sim *c) { unsigned v = fetch8(c); return v | (fetch8(c) << 8); }

static unsigned get8(Sim *c, int r) { return r < 4 ? (c->r[r] & 0xFF) : (c->r[r - 4] >> 8); }
static void set8(Sim *c, int r, unsigned v)
{
    if (r < 4) c->r[r] = (unsigned short)((c->r[r] & 0xFF00) | (v & 0xFF));
    else c->r[r - 4] = (unsigned short)((c->r[r - 4] & 0x00FF) | ((v & 0xFF) << 8));
}

static void push(Sim *c, unsigned v) { c->r[R_SP] -= 2; ww(c, c->s[S_SS], c->r[R_SP], v); }
static unsigned pop(Sim *c) { unsigned v = rw(c, c->s[S_SS], c->r[R_SP]); c->r[R_SP] += 2; return v; }

/* ------------------------------------------------------------------ operands */
static void modrm(Sim *c)
{
    unsigned b = fetch8(c), off = 0, cy = 0;
    int seg = S_DS;
    c->mod = b >> 6; c->reg = (b >> 3) & 7; c->rm = b & 7;
    if (c->mod == 3) { c->ea_cyc = 0; return; }
    switch (c->rm) {
    case 0: off = c->r[R_BX] + c->r[R_SI]; cy = 7; break;
    case 1: off = c->r[R_BX] + c->r[R_DI]; cy = 8; break;
    case 2: off = c->r[R_BP] + c->r[R_SI]; seg = S_SS; cy = 8; break;
    case 3: off = c->r[R_BP] + c->r[R_DI]; seg = S_SS; cy = 7; break;
    case 4: off = c->r[R_SI]; cy = 5; break;
    case 5: off = c->r[R_DI]; cy = 5; break;
    case 6:
        if (c->mod == 0) { off = fetch16(c); cy = 6; }
        else { off = c->r[R_BP]; seg = S_SS; cy = 5; }
        break;
    default: off = c->r[R_BX]; cy = 5; break;
    }
    if (c->mod == 1) { off += (unsigned)(int)(signed char)fetch8(c); cy += 4; }
    else if (c->mod == 2) { off += fetch16(c); cy += 4; }
    if (c->seg_ovr >= 0) { seg = c->seg_ovr; cy += 2; }
    c->ea_off = off & 0xFFFF;
    c->ea_seg = seg;
    c->ea_cyc = cy;
}
static unsigned rm8(Sim *c) { return c->mod == 3 ? get8(c, c->rm) : rb(c, c->s[c->ea_seg], c->ea_off); }
static unsigned rm16(Sim *c) { return c->mod == 3 ? c->r[c->rm] : rw(c, c->s[c->ea_seg], c->ea_off); }
static void setrm8(Sim *c, unsigned v) { if (c->mod == 3) set8(c, c->rm, v); else wb(c, c->s[c->ea_seg], c->ea_off, v); }
static void setrm16(Sim *c, unsigned v) { if (c->mod == 3) c->r[c->rm] = (unsigned short)v; else ww(c, c->s[c->ea_seg], c->ea_off, v); }
static unsigned rmget(Sim *c, int w) { return w ? rm16(c) : rm8(c); }
static void rmset(Sim *c, int w, unsigned v) { if (w) setrm16(c, v); else setrm8(c, v); }
static unsigned regget(Sim *c, int w, int r) { return w ? c->r[r] : get8(c, r); }
static void regset(Sim *c, int w, int r, unsigned v) { if (w) c->r[r] = (unsigned short)v; else set8(c, r, v); }

/* ------------------------------------------------------------------ flags / ALU */
static void szp(Sim *c, unsigned v, int w)
{
    c->fl &= ~(F_SF | F_ZF | F_PF);
    v &= w ? 0xFFFF : 0xFF;
    if (!v) c->fl |= F_ZF;
    if (v & (w ? 0x8000 : 0x80)) c->fl |= F_SF;
    if (parity[v & 0xFF]) c->fl |= F_PF;
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
static unsigned alu(Sim *c, int op, unsigned a, unsigned b, int w)
{
    unsigned mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80, res, cin = c->fl & F_CF;
    switch (op) {
    case 0: case 2:
        res = a + b + (op == 2 ? cin : 0);
        c->fl &= ~(F_CF | F_AF | F_OF);
        if (res > mask) c->fl |= F_CF;
        if ((a ^ b ^ res) & 0x10) c->fl |= F_AF;
        if ((a ^ res) & (b ^ res) & sign) c->fl |= F_OF;
        break;
    case 3: case 5: case 7:
        res = a - b - (op == 3 ? cin : 0);
        c->fl &= ~(F_CF | F_AF | F_OF);
        if (res & (mask + 1)) c->fl |= F_CF;
        if ((a ^ b ^ res) & 0x10) c->fl |= F_AF;
        if ((a ^ b) & (a ^ res) & sign) c->fl |= F_OF;
        break;
    case 1: res = a | b; c->fl &= ~(F_CF | F_OF | F_AF); break;
    case 4: res = a & b; c->fl &= ~(F_CF | F_OF | F_AF); break;
    default: res = a ^ b; c->fl &= ~(F_CF | F_OF | F_AF); break;
    }
    res &= mask;
    szp(c, res, w);
    return res;
}

static unsigned incdec(Sim *c, unsigned a, int dec, int w)
{
    unsigned mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    unsigned res = (dec ? a - 1 : a + 1) & mask;
    c->fl &= ~(F_AF | F_OF);
    if (dec) {
        if ((res & 0xF) == 0xF) c->fl |= F_AF;
        if (res == sign - 1) c->fl |= F_OF;
    } else {
        if ((res & 0xF) == 0) c->fl |= F_AF;
        if (res == sign) c->fl |= F_OF;
    }
    szp(c, res, w);
    return res;
}

/* op: 0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 7 SAR (6 is rejected before) */
static unsigned shift(Sim *c, int op, unsigned v, unsigned cnt, int w)
{
    unsigned bits = w ? 16 : 8, mask = w ? 0xFFFF : 0xFF, msb = w ? 0x8000 : 0x80;
    unsigned cf = (c->fl & F_CF) ? 1 : 0, i, of = 0, orig = v;
    if (cnt == 0) return v;
    switch (op) {
    case 0:
        for (i = 0; i < cnt; i++) { cf = (v & msb) ? 1 : 0; v = ((v << 1) | cf) & mask; }
        of = ((v & msb) ? 1 : 0) ^ cf;
        break;
    case 1:
        for (i = 0; i < cnt; i++) { cf = v & 1; v = (v >> 1) | (cf ? msb : 0); }
        of = ((v >> (bits - 1)) ^ (v >> (bits - 2))) & 1;
        break;
    case 2:
        for (i = 0; i < cnt; i++) { unsigned out = (v & msb) ? 1 : 0; v = ((v << 1) | cf) & mask; cf = out; }
        of = ((v & msb) ? 1 : 0) ^ cf;
        break;
    case 3:
        for (i = 0; i < cnt; i++) { unsigned out = v & 1; v = (v >> 1) | (cf ? msb : 0); cf = out; }
        of = ((v >> (bits - 1)) ^ (v >> (bits - 2))) & 1;
        break;
    case 4:
        for (i = 0; i < cnt; i++) { cf = (v & msb) ? 1 : 0; v = (v << 1) & mask; }
        of = ((v & msb) ? 1 : 0) ^ cf;
        break;
    case 5:
        for (i = 0; i < cnt; i++) { cf = v & 1; v >>= 1; }
        of = (orig & msb) ? 1 : 0;
        break;
    default:
        for (i = 0; i < cnt; i++) { cf = v & 1; v = (v >> 1) | (v & msb); }
        of = 0;
        break;
    }
    c->fl &= ~(F_CF | F_OF);
    if (cf) c->fl |= F_CF;
    if (of) c->fl |= F_OF;
    if (op >= 4) { c->fl &= ~F_AF; szp(c, v, w); }
    return v;
}

static int cond(Sim *c, int n)
{
    unsigned f = c->fl;
    int cf = !!(f & F_CF), zf = !!(f & F_ZF), sf = !!(f & F_SF), of = !!(f & F_OF), pf = !!(f & F_PF);
    int r;
    switch (n >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || (sf != of); break;
    }
    return (n & 1) ? !r : r;
}

/* ------------------------------------------------------------------ devices */
static void dm_driven(Sim *c, unsigned *red, unsigned *green, unsigned *col)
{
    Ppi *p = &c->p1;
    if (!p->set) { *red = *green = 0xFF; *col = 0; return; }
    *red = p->in_a ? 0xFF : p->latch[0];
    *green = p->in_b ? 0xFF : p->latch[1];
    *col = (p->in_cl ? 0 : (p->latch[2] & 0x0F)) | (p->in_cu ? 0 : (p->latch[2] & 0xF0));
}

static void dm_add(Sim *c, unsigned long long dt)
{
    unsigned red, green, col;
    int x, y;
    if (!dt) return;
    dm_driven(c, &red, &green, &col);
    if (!col || (red == 0xFF && green == 0xFF)) return;
    for (x = 0; x < 8; x++) {
        if (!((col >> x) & 1)) continue;
        for (y = 0; y < 8; y++) {
            if (!((green >> (7 - y)) & 1)) c->on_g[y * 8 + x] += dt;
            if (!((red >> (7 - y)) & 1)) c->on_r[y * 8 + x] += dt;
        }
    }
}

static void dm_window_done(Sim *c)
{
    unsigned long long mx = 0, g = 0, r = 0;
    int i;
    for (i = 0; i < 64; i++) {
        if (c->on_g[i] > mx) mx = c->on_g[i];
        if (c->on_r[i] > mx) mx = c->on_r[i];
    }
    if (mx * 400 >= c->win_len) {          /* brighter than 0.25 % of the time */
        for (i = 0; i < 64; i++) {
            if (c->on_g[i] * 100 >= mx * 35) g |= 1ULL << i;
            if (c->on_r[i] * 100 >= mx * 35) r |= 1ULL << i;
        }
    }
    memset(c->on_g, 0, sizeof c->on_g);
    memset(c->on_r, 0, sizeof c->on_r);
    if (!c->started) {
        if (!g && !r) return;              /* skip the dark time before the first picture */
        c->started = 1;
    }
    if (c->nwin < MAX_WINDOWS) {
        c->wg[c->nwin] = g;
        c->wr[c->nwin] = r;
        c->wt[c->nwin] = (double)c->win_start / SIM_CLOCK;
        c->nwin++;
    }
}

static void dm_advance(Sim *c, unsigned long long t)
{
    while (t >= c->win_start + c->win_len) {
        unsigned long long end = c->win_start + c->win_len;
        dm_add(c, end - c->last);
        c->last = end;
        dm_window_done(c);
        c->win_start = end;
    }
    dm_add(c, t - c->last);
    c->last = t;
}

static void seg_record(Sim *c, unsigned long long now)
{
    KitSimReport *r = c->rep;
    if (!c->seg_have) return;
    if ((double)(now - c->seg_since) < DIGIT_MIN * SIM_CLOCK) return;
    if (r->ndigits > 0 && r->digit[r->ndigits - 1] == (unsigned char)c->seg_cur) return;
    if (r->ndigits < SIM_MAXDIGITS) {
        r->digit[r->ndigits] = (unsigned char)c->seg_cur;
        r->digit_t[r->ndigits] = (double)c->seg_since / SIM_CLOCK;
        r->ndigits++;
    } else {
        r->digits_more = 1;
    }
}

static void seg_update(Sim *c)
{
    Ppi *p = &c->p2;
    int v = (!p->set || p->in_a) ? 0xFF : p->latch[0];
    if (c->seg_have && v == c->seg_cur) return;
    seg_record(c, c->cyc);
    c->seg_cur = v;
    c->seg_since = c->cyc;
    c->seg_have = 1;
}

static void note_port(Sim *c, unsigned port, int dir)
{
    KitSimReport *r = c->rep;
    int i;
    for (i = 0; i < r->nports; i++)
        if (r->port[i] == port) { r->port_dir[i] |= (unsigned char)dir; return; }
    if (r->nports < SIM_MAXPORTS) {
        r->port[r->nports] = (unsigned short)port;
        r->port_dir[r->nports] = (unsigned char)dir;
        r->nports++;
    }
}

static void ppi_write(Ppi *p, int idx, unsigned v)
{
    if (idx < 3) { p->latch[idx] = (unsigned char)v; return; }
    if (v & 0x80) {                         /* mode word */
        p->set = 1;
        p->in_a = (v >> 4) & 1;
        p->in_cu = (v >> 3) & 1;
        p->in_b = (v >> 1) & 1;
        p->in_cl = v & 1;
        p->latch[0] = p->latch[1] = p->latch[2] = 0;
    } else {                                /* bit set / reset on port C */
        int bit = (v >> 1) & 7;
        if (v & 1) p->latch[2] |= (unsigned char)(1 << bit);
        else p->latch[2] &= (unsigned char)~(1 << bit);
    }
}

static void port_out(Sim *c, unsigned port, unsigned v)
{
    KitSimReport *r = c->rep;
    port &= 0xFFFF;
    v &= 0xFF;
    if (port >= 0x18 && port <= 0x1F) {
        int idx = (port - 0x18) >> 1;       /* 0 A, 1 B, 2 C, 3 control */
        if (!(port & 1)) {                  /* CS1: dot matrix */
            dm_advance(c, c->cyc);
            if (idx < 3) {
                r->dm_used = 1;
                if (!c->p1.set) r->dm_before_init = 1;
            } else if (v & 0x80) {
                r->dm_init = 1;
            }
            ppi_write(&c->p1, idx, v);
        } else {                            /* CS2: 7-segment, LEDs, stepping motor */
            if (idx < 3) {
                if (idx == 0) r->seg_used = 1;
                else note_port(c, port, 1);
                if (!c->p2.set && idx == 0) r->seg_before_init = 1;
            } else if (v & 0x80) {
                r->seg_init = 1;
            }
            ppi_write(&c->p2, idx, v);
            seg_update(c);
        }
        return;
    }
    note_port(c, port, 1);
}

static unsigned port_in(Sim *c, unsigned port)
{
    port &= 0xFFFF;
    if (port >= 0x18 && port <= 0x1F) {
        Ppi *p = (port & 1) ? &c->p2 : &c->p1;
        int idx = (port - 0x18) >> 1;
        if (idx == 3) return 0xFF;
        if (!p->set) return 0xFF;
        if (idx == 0) return p->in_a ? 0xFF : p->latch[0];
        if (idx == 1) return p->in_b ? 0xFF : p->latch[1];
        return ((p->in_cl ? 0x0F : p->latch[2]) & 0x0F) | ((p->in_cu ? 0xF0 : p->latch[2]) & 0xF0);
    }
    note_port(c, port, 2);
    if (port == 0x02) return 0x00;          /* LCD status: not busy */
    if (port == 0x0A) return 0x05;          /* 8251 status: ready to send */
    return 0xFF;                            /* keypad: no key, others: nothing there */
}

/* ------------------------------------------------------------------ interrupts */
static void stop_here(Sim *c, int why)
{
    c->stop = why;
    c->ip = c->ip0;
    c->s[S_CS] = c->cs0;
}

static void interrupt(Sim *c, int n, int is_int3_insn)
{
    if (!c->vec[n]) {                       /* not the program's own handler */
        c->rep->stop_int = n;
        if (n == 3 && is_int3_insn) stop_here(c, SIM_INT3);
        else if (n == 0x21) stop_here(c, SIM_DOS);
        else if (n == 0) stop_here(c, SIM_DIV0);
        else stop_here(c, SIM_INT);
        return;
    }
    push(c, c->fl | 0xF000);
    push(c, c->s[S_CS]);
    push(c, c->ip);
    c->fl &= ~(F_IF | F_TF);
    c->ip = (unsigned short)rw(c, 0, n * 4);
    c->s[S_CS] = (unsigned short)rw(c, 0, n * 4 + 2);
    c->cyc += 51;
}

/* ------------------------------------------------------------------ string instructions */
static void string_once(Sim *c, int op)
{
    int w = op & 1;
    int d = (c->fl & F_DF) ? -(w ? 2 : 1) : (w ? 2 : 1);
    int sseg = c->seg_ovr >= 0 ? c->seg_ovr : S_DS;
    unsigned a, b;
    switch (op & 0xFE) {
    case 0xA4:      /* MOVS */
        if (w) ww(c, c->s[S_ES], c->r[R_DI], rw(c, c->s[sseg], c->r[R_SI]));
        else wb(c, c->s[S_ES], c->r[R_DI], rb(c, c->s[sseg], c->r[R_SI]));
        c->r[R_SI] += d; c->r[R_DI] += d;
        break;
    case 0xA6:      /* CMPS */
        a = w ? rw(c, c->s[sseg], c->r[R_SI]) : rb(c, c->s[sseg], c->r[R_SI]);
        b = w ? rw(c, c->s[S_ES], c->r[R_DI]) : rb(c, c->s[S_ES], c->r[R_DI]);
        alu(c, 7, a, b, w);
        c->r[R_SI] += d; c->r[R_DI] += d;
        break;
    case 0xAA:      /* STOS */
        if (w) ww(c, c->s[S_ES], c->r[R_DI], c->r[R_AX]);
        else wb(c, c->s[S_ES], c->r[R_DI], c->r[R_AX] & 0xFF);
        c->r[R_DI] += d;
        break;
    case 0xAC:      /* LODS */
        if (w) c->r[R_AX] = (unsigned short)rw(c, c->s[sseg], c->r[R_SI]);
        else set8(c, 0, rb(c, c->s[sseg], c->r[R_SI]));
        c->r[R_SI] += d;
        break;
    default:        /* SCAS */
        b = w ? rw(c, c->s[S_ES], c->r[R_DI]) : rb(c, c->s[S_ES], c->r[R_DI]);
        alu(c, 7, w ? c->r[R_AX] : (c->r[R_AX] & 0xFF), b, w);
        c->r[R_DI] += d;
        break;
    }
}

static void string_op(Sim *c, int op)
{
    /* clocks:              MOVS CMPS STOS LODS SCAS */
    static const unsigned char once[5] = { 18, 22, 11, 12, 15 };
    static const unsigned char each[5] = { 17, 22, 10, 13, 15 };
    int k, cmp;
    switch (op & 0xFE) {
    case 0xA4: k = 0; break;
    case 0xA6: k = 1; break;
    case 0xAA: k = 2; break;
    case 0xAC: k = 3; break;
    default:   k = 4; break;
    }
    cmp = (k == 1 || k == 4);
    if (!c->repx) {
        string_once(c, op);
        c->cyc += once[k];
        return;
    }
    c->cyc += 9;
    while (c->r[R_CX]) {
        string_once(c, op);
        c->r[R_CX]--;
        c->cyc += each[k];
        if (cmp) {
            if (c->repx == 0xF3 && !(c->fl & F_ZF)) break;
            if (c->repx == 0xF2 && (c->fl & F_ZF)) break;
        }
    }
}

/* ------------------------------------------------------------------ one instruction */
static void step(Sim *c)
{
    unsigned op, a, b, w, t, here;
    int prefix_done = 0;

    c->ip0 = c->ip;
    c->cs0 = c->s[S_CS];
    here = phys(c->s[S_CS], c->ip);
    if (here < c->lo || here >= c->hi) {
        c->stop = here >= 0xF0000 ? SIM_ROM : SIM_OUTSIDE;
        return;
    }
    c->seg_ovr = -1;
    c->repx = 0;
    while (!prefix_done) {
        op = fetch8(c);
        switch (op) {
        case 0x26: c->seg_ovr = S_ES; c->cyc += 2; break;
        case 0x2E: c->seg_ovr = S_CS; c->cyc += 2; break;
        case 0x36: c->seg_ovr = S_SS; c->cyc += 2; break;
        case 0x3E: c->seg_ovr = S_DS; c->cyc += 2; break;
        case 0xF0: c->cyc += 2; break;
        case 0xF2: case 0xF3: c->repx = (int)op; c->cyc += 2; break;
        default: prefix_done = 1; break;
        }
    }
    c->insns++;

    if (op < 0x40 && (op & 7) < 6) {        /* ALU r/m,reg  reg,r/m  acc,imm */
        int aop = op >> 3;
        w = op & 1;
        if ((op & 7) < 4) {
            int d = op & 2;
            modrm(c);
            if (d) {
                a = regget(c, w, c->reg); b = rmget(c, w);
                t = alu(c, aop, a, b, w);
                if (aop != 7) regset(c, w, c->reg, t);
                c->cyc += c->mod == 3 ? 3 : 9 + c->ea_cyc;
            } else {
                a = rmget(c, w); b = regget(c, w, c->reg);
                t = alu(c, aop, a, b, w);
                if (aop != 7) rmset(c, w, t);
                c->cyc += c->mod == 3 ? 3 : (aop == 7 ? 9 : 16) + c->ea_cyc;
            }
        } else {
            b = w ? fetch16(c) : fetch8(c);
            a = w ? c->r[R_AX] : (c->r[R_AX] & 0xFF);
            t = alu(c, aop, a, b, w);
            if (aop != 7) regset(c, w, 0, t);
            c->cyc += 4;
        }
        return;
    }

    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        push(c, c->s[(op >> 3) & 3]); c->cyc += 10; break;
    case 0x07: case 0x17: case 0x1F:
        c->s[(op >> 3) & 3] = (unsigned short)pop(c); c->cyc += 8; break;
    case 0x0F:
        stop_here(c, SIM_BADOP); c->rep->stop_op = (int)op; break;
    case 0x27: {    /* DAA */
        unsigned al = c->r[R_AX] & 0xFF, oal = al, ocf = c->fl & F_CF;
        c->fl &= ~F_CF;
        if ((al & 0xF) > 9 || (c->fl & F_AF)) {
            al += 6;
            if (ocf || al > 0xFF) c->fl |= F_CF;
            c->fl |= F_AF;
        } else c->fl &= ~F_AF;
        if (oal > 0x99 || ocf) { al += 0x60; c->fl |= F_CF; } else c->fl &= ~F_CF;
        set8(c, 0, al); szp(c, al, 0); c->cyc += 4; break;
    }
    case 0x2F: {    /* DAS */
        unsigned al = c->r[R_AX] & 0xFF, oal = al, ocf = c->fl & F_CF;
        c->fl &= ~F_CF;
        if ((al & 0xF) > 9 || (c->fl & F_AF)) {
            if (ocf || al < 6) c->fl |= F_CF;
            al -= 6;
            c->fl |= F_AF;
        } else c->fl &= ~F_AF;
        if (oal > 0x99 || ocf) { al -= 0x60; c->fl |= F_CF; }
        set8(c, 0, al); szp(c, al, 0); c->cyc += 4; break;
    }
    case 0x37: case 0x3F: {    /* AAA AAS */
        unsigned al = c->r[R_AX] & 0xFF, ah = c->r[R_AX] >> 8;
        if ((al & 0xF) > 9 || (c->fl & F_AF)) {
            if (op == 0x37) { al += 6; ah += 1; } else { al -= 6; ah -= 1; }
            c->fl |= F_AF | F_CF;
        } else c->fl &= ~(F_AF | F_CF);
        c->r[R_AX] = (unsigned short)(((ah & 0xFF) << 8) | (al & 0x0F));
        c->cyc += 4; break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        c->r[op & 7] = (unsigned short)incdec(c, c->r[op & 7], op >= 0x48, 1); c->cyc += 2; break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (op == 0x54) push(c, (c->r[R_SP] - 2) & 0xFFFF);   /* 8086 pushes the new SP */
        else push(c, c->r[op & 7]);
        c->cyc += 11; break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        t = pop(c); c->r[op & 7] = (unsigned short)t; c->cyc += 8; break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int d = (signed char)fetch8(c);
        if (cond(c, op & 15)) { c->ip = (unsigned short)(c->ip + d); c->cyc += 16; }
        else c->cyc += 4;
        break;
    }
    case 0x80: case 0x81: case 0x82: case 0x83:
        w = op & 1;
        modrm(c);
        a = rmget(c, w);
        if (op == 0x83) b = (unsigned)(int)(signed char)fetch8(c) & 0xFFFF;
        else b = w ? fetch16(c) : fetch8(c);
        t = alu(c, c->reg, a, b, w);
        if (c->reg != 7) rmset(c, w, t);
        c->cyc += c->mod == 3 ? 4 : (c->reg == 7 ? 10 : 17) + c->ea_cyc;
        break;
    case 0x84: case 0x85:
        w = op & 1; modrm(c);
        alu(c, 4, rmget(c, w), regget(c, w, c->reg), w);
        c->cyc += c->mod == 3 ? 3 : 9 + c->ea_cyc;
        break;
    case 0x86: case 0x87:
        w = op & 1; modrm(c);
        a = rmget(c, w); b = regget(c, w, c->reg);
        rmset(c, w, b); regset(c, w, c->reg, a);
        c->cyc += c->mod == 3 ? 4 : 17 + c->ea_cyc;
        break;
    case 0x88: case 0x89:
        w = op & 1; modrm(c);
        rmset(c, w, regget(c, w, c->reg));
        c->cyc += c->mod == 3 ? 2 : 9 + c->ea_cyc;
        break;
    case 0x8A: case 0x8B:
        w = op & 1; modrm(c);
        regset(c, w, c->reg, rmget(c, w));
        c->cyc += c->mod == 3 ? 2 : 8 + c->ea_cyc;
        break;
    case 0x8C:
        modrm(c);
        setrm16(c, c->s[c->reg & 3]);
        c->cyc += c->mod == 3 ? 2 : 9 + c->ea_cyc;
        break;
    case 0x8D:
        modrm(c);
        if (c->mod == 3) { stop_here(c, SIM_BADOP); c->rep->stop_op = (int)op; break; }
        c->r[c->reg] = (unsigned short)c->ea_off;
        c->cyc += 2 + c->ea_cyc;
        break;
    case 0x8E:
        modrm(c);
        c->s[c->reg & 3] = (unsigned short)rm16(c);
        c->cyc += c->mod == 3 ? 2 : 8 + c->ea_cyc;
        break;
    case 0x8F:
        modrm(c);
        t = pop(c);
        setrm16(c, t);
        c->cyc += c->mod == 3 ? 8 : 17 + c->ea_cyc;
        break;
    case 0x90: c->cyc += 3; break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        t = c->r[R_AX]; c->r[R_AX] = c->r[op & 7]; c->r[op & 7] = (unsigned short)t; c->cyc += 3; break;
    case 0x98: c->r[R_AX] = (unsigned short)(int)(signed char)(c->r[R_AX] & 0xFF); c->cyc += 2; break;
    case 0x99: c->r[R_DX] = (c->r[R_AX] & 0x8000) ? 0xFFFF : 0; c->cyc += 5; break;
    case 0x9A: {
        unsigned off = fetch16(c), seg = fetch16(c);
        push(c, c->s[S_CS]); push(c, c->ip);
        c->s[S_CS] = (unsigned short)seg; c->ip = (unsigned short)off; c->cyc += 28; break;
    }
    case 0x9B: c->cyc += 3; break;
    case 0x9C: push(c, c->fl | 0xF000); c->cyc += 10; break;
    case 0x9D: c->fl = (unsigned short)((pop(c) & 0x0FD5) | 0x0002); c->cyc += 8; break;
    case 0x9E: c->fl = (unsigned short)((c->fl & 0xFF00) | ((c->r[R_AX] >> 8) & 0xD5) | 0x02); c->cyc += 4; break;
    case 0x9F: set8(c, 4, (c->fl & 0xD5) | 0x02); c->cyc += 4; break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        unsigned off = fetch16(c);
        int seg = c->seg_ovr >= 0 ? c->seg_ovr : S_DS;
        w = op & 1;
        if (op < 0xA2) {
            if (w) c->r[R_AX] = (unsigned short)rw(c, c->s[seg], off);
            else set8(c, 0, rb(c, c->s[seg], off));
        } else {
            if (w) ww(c, c->s[seg], off, c->r[R_AX]);
            else wb(c, c->s[seg], off, c->r[R_AX] & 0xFF);
        }
        c->cyc += 10; break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, (int)op); break;
    case 0xA8: alu(c, 4, c->r[R_AX] & 0xFF, fetch8(c), 0); c->cyc += 4; break;
    case 0xA9: alu(c, 4, c->r[R_AX], fetch16(c), 1); c->cyc += 4; break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set8(c, op & 7, fetch8(c)); c->cyc += 4; break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        c->r[op & 7] = (unsigned short)fetch16(c); c->cyc += 4; break;
    case 0xC2: case 0xC3: {
        unsigned n = op == 0xC2 ? fetch16(c) : 0;
        c->ip = (unsigned short)pop(c);
        c->r[R_SP] += (unsigned short)n;
        c->cyc += op == 0xC2 ? 12 : 8; break;
    }
    case 0xC4: case 0xC5:
        modrm(c);
        if (c->mod == 3) { stop_here(c, SIM_BADOP); c->rep->stop_op = (int)op; break; }
        c->r[c->reg] = (unsigned short)rw(c, c->s[c->ea_seg], c->ea_off);
        c->s[op == 0xC4 ? S_ES : S_DS] = (unsigned short)rw(c, c->s[c->ea_seg], c->ea_off + 2);
        c->cyc += 16 + c->ea_cyc; break;
    case 0xC6: case 0xC7:
        w = op & 1; modrm(c);
        t = w ? fetch16(c) : fetch8(c);
        rmset(c, w, t);
        c->cyc += c->mod == 3 ? 4 : 10 + c->ea_cyc; break;
    case 0xCA: case 0xCB: {
        unsigned n = op == 0xCA ? fetch16(c) : 0;
        c->ip = (unsigned short)pop(c);
        c->s[S_CS] = (unsigned short)pop(c);
        c->r[R_SP] += (unsigned short)n;
        c->cyc += op == 0xCA ? 17 : 18; break;
    }
    case 0xCC: interrupt(c, 3, 1); break;
    case 0xCD: interrupt(c, (int)fetch8(c), 0); break;
    case 0xCE: if (c->fl & F_OF) interrupt(c, 4, 0); else c->cyc += 4; break;
    case 0xCF:
        c->ip = (unsigned short)pop(c);
        c->s[S_CS] = (unsigned short)pop(c);
        c->fl = (unsigned short)((pop(c) & 0x0FD5) | 0x0002);
        c->cyc += 24; break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        unsigned cnt;
        w = op & 1; modrm(c);
        if (c->reg == 6) { stop_here(c, SIM_BADOP); c->rep->stop_op = (int)op; break; }
        cnt = (op & 2) ? (c->r[R_CX] & 0xFF) : 1;
        a = rmget(c, w);
        t = shift(c, c->reg, a, cnt, w);
        rmset(c, w, t);
        if (op & 2) c->cyc += (c->mod == 3 ? 8 : 20 + c->ea_cyc) + 4 * cnt;
        else c->cyc += c->mod == 3 ? 2 : 15 + c->ea_cyc;
        break;
    }
    case 0xD4: {    /* AAM */
        unsigned base = fetch8(c), al = c->r[R_AX] & 0xFF;
        if (!base) { interrupt(c, 0, 0); break; }
        c->r[R_AX] = (unsigned short)(((al / base) << 8) | (al % base));
        szp(c, al % base, 0); c->cyc += 83; break;
    }
    case 0xD5: {    /* AAD */
        unsigned base = fetch8(c), al = c->r[R_AX] & 0xFF, ah = c->r[R_AX] >> 8;
        al = (al + ah * base) & 0xFF;
        c->r[R_AX] = (unsigned short)al;
        szp(c, al, 0); c->cyc += 60; break;
    }
    case 0xD7: {    /* XLAT */
        int seg = c->seg_ovr >= 0 ? c->seg_ovr : S_DS;
        set8(c, 0, rb(c, c->s[seg], (c->r[R_BX] + (c->r[R_AX] & 0xFF)) & 0xFFFF));
        c->cyc += 11; break;
    }
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        int d = (signed char)fetch8(c), jump;
        if (op == 0xE3) { jump = c->r[R_CX] == 0; c->cyc += jump ? 18 : 6; }
        else {
            c->r[R_CX]--;
            jump = c->r[R_CX] != 0;
            if (op == 0xE0) jump = jump && !(c->fl & F_ZF);
            if (op == 0xE1) jump = jump && (c->fl & F_ZF);
            c->cyc += op == 0xE2 ? (jump ? 17 : 5) : (jump ? 19 : 5);
        }
        if (jump) c->ip = (unsigned short)(c->ip + d);
        break;
    }
    case 0xE4: case 0xE5: {
        unsigned p = fetch8(c);
        set8(c, 0, port_in(c, p));
        if (op & 1) set8(c, 4, port_in(c, p + 1));
        c->cyc += (op & 1) ? 14 : 10; break;
    }
    case 0xE6: case 0xE7: {
        unsigned p = fetch8(c);
        c->cyc += (op & 1) ? 14 : 10;
        port_out(c, p, c->r[R_AX] & 0xFF);
        if (op & 1) port_out(c, p + 1, c->r[R_AX] >> 8);
        break;
    }
    case 0xE8: {
        unsigned d = fetch16(c);
        push(c, c->ip);
        c->ip = (unsigned short)(c->ip + d); c->cyc += 19; break;
    }
    case 0xE9: { unsigned d = fetch16(c); c->ip = (unsigned short)(c->ip + d); c->cyc += 15; break; }
    case 0xEA: {
        unsigned off = fetch16(c), seg = fetch16(c);
        c->ip = (unsigned short)off; c->s[S_CS] = (unsigned short)seg; c->cyc += 15; break;
    }
    case 0xEB: { int d = (signed char)fetch8(c); c->ip = (unsigned short)(c->ip + d); c->cyc += 15; break; }
    case 0xEC: case 0xED:
        set8(c, 0, port_in(c, c->r[R_DX]));
        if (op & 1) set8(c, 4, port_in(c, c->r[R_DX] + 1));
        c->cyc += (op & 1) ? 12 : 8; break;
    case 0xEE: case 0xEF:
        c->cyc += (op & 1) ? 12 : 8;
        port_out(c, c->r[R_DX], c->r[R_AX] & 0xFF);
        if (op & 1) port_out(c, c->r[R_DX] + 1, c->r[R_AX] >> 8);
        break;
    case 0xF4: stop_here(c, SIM_HLT); break;
    case 0xF5: c->fl ^= F_CF; c->cyc += 2; break;
    case 0xF6: case 0xF7:
        w = op & 1; modrm(c);
        a = rmget(c, w);
        switch (c->reg) {
        case 0: case 1:
            b = w ? fetch16(c) : fetch8(c);
            alu(c, 4, a, b, w);
            c->cyc += c->mod == 3 ? 5 : 11 + c->ea_cyc; break;
        case 2:
            rmset(c, w, ~a & (w ? 0xFFFF : 0xFF));
            c->cyc += c->mod == 3 ? 3 : 16 + c->ea_cyc; break;
        case 3:
            t = alu(c, 5, 0, a, w);
            if (a) c->fl |= F_CF; else c->fl &= ~F_CF;
            rmset(c, w, t);
            c->cyc += c->mod == 3 ? 3 : 16 + c->ea_cyc; break;
        case 4:     /* MUL */
            if (!w) {
                unsigned r = (c->r[R_AX] & 0xFF) * a;
                c->r[R_AX] = (unsigned short)r;
                if (r & 0xFF00) c->fl |= F_CF | F_OF; else c->fl &= ~(F_CF | F_OF);
                c->cyc += 74;
            } else {
                unsigned long r = (unsigned long)c->r[R_AX] * a;
                c->r[R_AX] = (unsigned short)(r & 0xFFFF);
                c->r[R_DX] = (unsigned short)(r >> 16);
                if (c->r[R_DX]) c->fl |= F_CF | F_OF; else c->fl &= ~(F_CF | F_OF);
                c->cyc += 126;
            }
            if (c->mod != 3) c->cyc += 6 + c->ea_cyc;
            break;
        case 5:     /* IMUL */
            if (!w) {
                int r = (signed char)(c->r[R_AX] & 0xFF) * (signed char)a;
                c->r[R_AX] = (unsigned short)(r & 0xFFFF);
                if (r != (signed char)(r & 0xFF)) c->fl |= F_CF | F_OF; else c->fl &= ~(F_CF | F_OF);
                c->cyc += 89;
            } else {
                long r = (long)(short)c->r[R_AX] * (long)(short)a;
                c->r[R_AX] = (unsigned short)(r & 0xFFFF);
                c->r[R_DX] = (unsigned short)((r >> 16) & 0xFFFF);
                if (r != (short)(r & 0xFFFF)) c->fl |= F_CF | F_OF; else c->fl &= ~(F_CF | F_OF);
                c->cyc += 141;
            }
            if (c->mod != 3) c->cyc += 6 + c->ea_cyc;
            break;
        case 6:     /* DIV */
            if (!w) {
                unsigned n = c->r[R_AX], q;
                if (!a || (q = n / a) > 0xFF) { interrupt(c, 0, 0); break; }
                c->r[R_AX] = (unsigned short)(((n % a) << 8) | q);
                c->cyc += 85;
            } else {
                unsigned long n = ((unsigned long)c->r[R_DX] << 16) | c->r[R_AX], q;
                if (!a || (q = n / a) > 0xFFFF) { interrupt(c, 0, 0); break; }
                c->r[R_AX] = (unsigned short)q;
                c->r[R_DX] = (unsigned short)(n % a);
                c->cyc += 153;
            }
            if (c->mod != 3) c->cyc += 6 + c->ea_cyc;
            break;
        default:    /* IDIV */
            if (!w) {
                long n = (short)c->r[R_AX], d = (signed char)a, q;
                if (!d) { interrupt(c, 0, 0); break; }
                q = n / d;
                if (q > 127 || q < -127) { interrupt(c, 0, 0); break; }
                c->r[R_AX] = (unsigned short)((((n % d) & 0xFF) << 8) | (q & 0xFF));
                c->cyc += 107;
            } else {
                long long n = (long long)(long)(((unsigned long)c->r[R_DX] << 16) | c->r[R_AX]);
                long long d = (short)a, q;
                if (n > 0x7FFFFFFFLL) n -= 0x100000000LL;
                if (!d) { interrupt(c, 0, 0); break; }
                q = n / d;
                if (q > 32767 || q < -32767) { interrupt(c, 0, 0); break; }
                c->r[R_AX] = (unsigned short)(q & 0xFFFF);
                c->r[R_DX] = (unsigned short)((n % d) & 0xFFFF);
                c->cyc += 175;
            }
            if (c->mod != 3) c->cyc += 6 + c->ea_cyc;
            break;
        }
        break;
    case 0xF8: c->fl &= ~F_CF; c->cyc += 2; break;
    case 0xF9: c->fl |= F_CF; c->cyc += 2; break;
    case 0xFA: c->fl &= ~F_IF; c->cyc += 2; break;
    case 0xFB: c->fl |= F_IF; c->cyc += 2; break;
    case 0xFC: c->fl &= ~F_DF; c->cyc += 2; break;
    case 0xFD: c->fl |= F_DF; c->cyc += 2; break;
    case 0xFE:
        modrm(c);
        if (c->reg > 1) { stop_here(c, SIM_BADOP); c->rep->stop_op = (int)op; break; }
        setrm8(c, incdec(c, rm8(c), c->reg == 1, 0));
        c->cyc += c->mod == 3 ? 3 : 15 + c->ea_cyc;
        break;
    case 0xFF:
        modrm(c);
        switch (c->reg) {
        case 0: case 1:
            setrm16(c, incdec(c, rm16(c), c->reg == 1, 1));
            c->cyc += c->mod == 3 ? 2 : 15 + c->ea_cyc; break;
        case 2:
            t = rm16(c); push(c, c->ip); c->ip = (unsigned short)t;
            c->cyc += c->mod == 3 ? 16 : 21 + c->ea_cyc; break;
        case 3:
            if (c->mod == 3) { stop_here(c, SIM_BADOP); c->rep->stop_op = (int)op; break; }
            a = rw(c, c->s[c->ea_seg], c->ea_off); b = rw(c, c->s[c->ea_seg], c->ea_off + 2);
            push(c, c->s[S_CS]); push(c, c->ip);
            c->ip = (unsigned short)a; c->s[S_CS] = (unsigned short)b;
            c->cyc += 37 + c->ea_cyc; break;
        case 4:
            c->ip = (unsigned short)rm16(c);
            c->cyc += c->mod == 3 ? 11 : 18 + c->ea_cyc; break;
        case 5:
            if (c->mod == 3) { stop_here(c, SIM_BADOP); c->rep->stop_op = (int)op; break; }
            a = rw(c, c->s[c->ea_seg], c->ea_off); b = rw(c, c->s[c->ea_seg], c->ea_off + 2);
            c->ip = (unsigned short)a; c->s[S_CS] = (unsigned short)b;
            c->cyc += 24 + c->ea_cyc; break;
        case 6:
            push(c, rm16(c));
            c->cyc += c->mod == 3 ? 11 : 16 + c->ea_cyc; break;
        default:
            stop_here(c, SIM_BADOP); c->rep->stop_op = (int)op; break;
        }
        break;
    default:
        /* 60-6F, C0, C1, C8, C9: 80186+;  D8-DF: 8087;  D6, F1: not 8086 instructions */
        if ((op >= 0x60 && op <= 0x6F) || op == 0xC0 || op == 0xC1 || op == 0xC8 || op == 0xC9 ||
            (op >= 0xD8 && op <= 0xDF))
            stop_here(c, SIM_NOT8086);
        else
            stop_here(c, SIM_BADOP);
        c->rep->stop_op = (int)op;
        break;
    }
}

/* ------------------------------------------------------------------ pictures */
static void make_frames(Sim *c)
{
    KitSimReport *r = c->rep;
    int i, n = c->nwin, stable_any = 0;
    unsigned long long lastg = 0, lastr = 0;
    int have_last = 0, lit_total = 0, lit_windows = 0;

    for (i = 0; i + 1 < n; i++)
        if (c->wg[i] == c->wg[i + 1] && c->wr[i] == c->wr[i + 1]) { stable_any = 1; break; }

    for (i = 0; i < n; i++) {
        unsigned long long g = c->wg[i], rr = c->wr[i];
        int stable = 1;
        if (stable_any) {   /* skip windows that mix two pictures (the change happened inside) */
            stable = (i > 0 && c->wg[i - 1] == g && c->wr[i - 1] == rr) ||
                     (i + 1 < n && c->wg[i + 1] == g && c->wr[i + 1] == rr);
        }
        if (!stable) continue;
        if (g | rr) {
            /* mistakes that light (almost) everything: one colour's rows left at 0 (= on),
               e.g. red port not set to 0FFH, or a table with 1 = on, or DS wrong. A full
               picture made of several colours (e.g. coloured rows) is not a mistake. */
            int k, cnt, ng = 0, nr = 0, nb = 0, most;
            for (k = 0; k < 64; k++) {
                int gg = (int)((g >> k) & 1), rd = (int)((rr >> k) & 1);
                if (gg && rd) nb++; else if (gg) ng++; else if (rd) nr++;
            }
            cnt = ng + nr + nb;
            most = ng > nr ? ng : nr;
            if (nb > most) most = nb;
            lit_windows++;
            if (ng + nb >= 56 || nr + nb >= 56 || (cnt >= 48 && most * 10 >= cnt * 9)) lit_total++;
            if (rr) r->dm_red = 1;
        }
        if (have_last && g == lastg && rr == lastr) continue;
        if (have_last) r->nchanges++;
        have_last = 1; lastg = g; lastr = rr;
        if (r->nframes < SIM_MAXFRAMES) {
            int x, y;
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x++) {
                    int bit = y * 8 + x, gg = (int)((g >> bit) & 1), rd = (int)((rr >> bit) & 1);
                    r->frame[r->nframes][y][x] = gg && rd ? '@' : gg ? '#' : rd ? 'R' : '.';
                }
                r->frame[r->nframes][y][8] = 0;
            }
            r->frame_t[r->nframes] = c->wt[i];
            r->nframes++;
        } else {
            r->frames_more = 1;
        }
    }
    if (lit_windows && lit_total * 2 > lit_windows) r->dm_mostly_on = 1;
}

static void init_tables(void)
{
    int i, k, ones;
    if (parity[0]) return;
    for (i = 0; i < 256; i++) {
        for (k = 0, ones = 0; k < 8; k++) ones += (i >> k) & 1;
        parity[i] = (unsigned char)!(ones & 1);
    }
}

#ifdef KITSIM_TEST
/* CPU test hook: run up to n instructions on the caller's 1 MB memory.
   regs: AX CX DX BX SP BP SI DI ES CS SS DS IP FLAGS (updated). Returns the stop code or -1. */
int kitsim_test(unsigned char *mem, unsigned short *regs, unsigned lo, unsigned hi,
                unsigned long n, unsigned long long *cycles)
{
    static KitSimReport rep;
    Sim *c = (Sim *)calloc(1, sizeof *c);
    int i, stop;
    init_tables();
    memset(&rep, 0, sizeof rep);
    c->m = mem; c->rep = &rep; c->stop = -1; c->lo = lo; c->hi = hi;
    c->win_len = (unsigned long long)(WIN_SEC * SIM_CLOCK);
    for (i = 0; i < 8; i++) c->r[i] = regs[i];
    for (i = 0; i < 4; i++) c->s[i] = regs[8 + i];
    c->ip = regs[12]; c->fl = regs[13];
    while (c->stop < 0 && c->insns < n) step(c);
    for (i = 0; i < 8; i++) regs[i] = c->r[i];
    for (i = 0; i < 4; i++) regs[8 + i] = c->s[i];
    regs[12] = c->ip; regs[13] = c->fl;
    if (cycles) *cycles = c->cyc;
    stop = c->stop;
    free(c);
    return stop;
}
#endif

void kit_simulate(const unsigned char *code, int len, unsigned base, double seconds,
                  KitSimReport *r)
{
    Sim *c;
    unsigned long long limit;
    unsigned long max_insns;

    memset(r, 0, sizeof *r);
    r->stop = SIM_TIMEUP;
    init_tables();
    c = (Sim *)calloc(1, sizeof *c);
    if (!c) return;
    c->m = (unsigned char *)calloc(1, 0x100000);
    if (!c->m) { free(c); return; }
    if (len < 0) len = 0;
    if ((unsigned)len > 0x100000 - base) len = (int)(0x100000 - base);
    memcpy(c->m + base, code, len);
    memset(c->m + 0xF0000, 0xFF, 0x10000);
    c->lo = base;
    c->hi = base + (unsigned)len;
    c->rep = r;
    c->stop = -1;
    c->ip = (unsigned short)base;
    c->r[R_SP] = 0x0540;
    c->fl = 0x0002;
    c->win_len = (unsigned long long)(WIN_SEC * SIM_CLOCK);
    limit = (unsigned long long)(seconds * SIM_CLOCK);
    max_insns = (unsigned long)(seconds * 1500000.0) + 100000;   /* about 3 clocks each, at most */

    while (c->stop < 0 && c->cyc < limit && c->insns < max_insns)
        step(c);

    r->stop = c->stop < 0 ? SIM_TIMEUP : c->stop;
    r->stop_cs = c->s[S_CS];
    r->stop_ip = c->ip;
    r->seconds = (double)c->cyc / SIM_CLOCK;
    r->insns = c->insns;

    /* a program that stopped keeps its last output: show that for a while */
    {
        unsigned long long end = c->cyc;
        if (c->stop >= 0 || end < c->win_len * 3) end = c->cyc + c->win_len * 3;
        dm_advance(c, end);
        if (end - c->win_start >= c->win_len / 2) dm_window_done(c);
        c->cyc = end;
        seg_record(c, end);
    }
    make_frames(c);
    free(c->m);
    free(c);
}

/* ------------------------------------------------------------------ text */
typedef struct { char *buf; size_t len, cap; } Out;

#ifdef __GNUC__
static void put(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static void put(Out *o, const char *fmt, ...)
{
    char tmp[600];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
    if (o->len + n + 2 > o->cap) {
        size_t cap = (o->cap + n + 2) * 2;
        char *nb = (char *)realloc(o->buf, cap);
        if (!nb) return;
        o->buf = nb; o->cap = cap;
    }
    memcpy(o->buf + o->len, tmp, n);
    o->len += n;
    o->buf[o->len] = 0;
}

/* name of a port that is not one of the two 8255s */
static const char *port_name(unsigned p, int *exists)
{
    *exists = 1;
    switch (p) {
    case 0x00: return "LCD instruction register";
    case 0x01: return "keypad";
    case 0x02: return "LCD status register";
    case 0x04: return "LCD data register";
    case 0x08: return "8251 data (the serial link to the PC)";
    case 0x0A: return "8251 control (the serial link to the PC)";
    case 0x09: return "8253 timer 0";
    case 0x0B: return "8253 timer 1";
    case 0x0D: return "8253 timer 2";
    case 0x0F: return "8253 control";
    case 0x10: return "8259 command";
    case 0x11: return "speaker";
    case 0x12: return "8259 data";
    case 0x1B: return "8255 CS2 port B (LEDs / stepping motor)";
    case 0x1D: return "8255 CS2 port C (stepping motor)";
    }
    if (p >= 0x20 && p <= 0x2F) { *exists = 2; return "I/O extension connector"; }
    *exists = 0;
    return "nothing is connected there on the MDA-8086";
}

/* 7-segment code (0 = segment on) -> "7", "8.", "(blank)" or "[xxH]" if it is no known sign */
static void seg_text(unsigned v, char *out)
{
    static const unsigned char pat[] = { 0xC0, 0xF9, 0xA4, 0xB0, 0x99, 0x92, 0x82, 0xF8, 0x80, 0x90,
                                         0x88, 0x83, 0xC6, 0xA1, 0x86, 0x8E, 0xBF, 0xFF };
    static const char *txt[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
                                 "A", "b", "C", "d", "E", "F", "-", "(blank)" };
    unsigned base = v | 0x80;               /* without the decimal point */
    int n = (int)sizeof pat, i, dp = !(v & 0x80);
    if (base == 0xD8) base = 0xF8;          /* 7 drawn with segment f */
    if (base == 0x98) base = 0x90;          /* 9 drawn without segment d */
    for (i = 0; i < n; i++) if (pat[i] == base) break;
    if (i == n) { sprintf(out, "[%02XH]", v); return; }
    if (base == 0xFF && dp) { strcpy(out, "."); return; }
    strcpy(out, txt[i]);
    if (dp) strcat(out, ".");
}

static int is_digit_code(unsigned v)
{
    static const unsigned char pat[] = { 0xC0, 0xF9, 0xA4, 0xB0, 0x99, 0x92, 0x82, 0xF8, 0x80, 0x90 };
    int i;
    for (i = 0; i < 10; i++) if ((v | 0x80) == pat[i]) return 1;
    return 0;
}

char *kit_sim_text(const KitSimReport *r, int picture)
{
    Out o = { NULL, 0, 0 };
    int i;


    put(&o, "%s", "");
    if (picture) {
        put(&o, "Kit check (program run on a model of the kit for %.1f s):\n", r->seconds);
        if (r->nframes == 1) {
            put(&o, "  Dot matrix, steady picture:\n");
            for (i = 0; i < 8; i++) put(&o, "    %s\n", r->frame[0][i]);
        } else if (r->nframes > 1) {
            int k, y;
            put(&o, "  Dot matrix, moving picture (%d changes; first %d shown, time in s):\n",
                r->nchanges, r->nframes);
            put(&o, "  ");
            for (k = 0; k < r->nframes; k++) put(&o, "  %-8.1f", r->frame_t[k]);
            put(&o, "\n");
            for (y = 0; y < 8; y++) {
                put(&o, "  ");
                for (k = 0; k < r->nframes; k++) put(&o, "  %s", r->frame[k][y]);
                put(&o, "\n");
            }
        }
        if (r->dm_red) put(&o, "    (# = green, R = red, @ = both = yellow)\n");
        if (r->ndigits > 0) {
            char t[16];
            put(&o, "  7-segment shows:");
            for (i = 0; i < r->ndigits; i++) { seg_text(r->digit[i], t); put(&o, " %s", t); }
            if (r->digits_more) put(&o, " ...");
            if (r->ndigits >= 3) {
                double each = (r->digit_t[r->ndigits - 1] - r->digit_t[0]) / (r->ndigits - 1);
                put(&o, "   (about %.2f s each)", each);
            }
            put(&o, "\n");
        }
        if (!r->dm_used && !r->seg_used && !r->nports)
            put(&o, "  Nothing was sent to the dot matrix or the 7-segment.\n");
    }
    if (!r->dm_used && !r->seg_used && r->nports)
        put(&o, "!Nothing reached the dot matrix or the 7-segment. Their ports: dot matrix 18H/1AH/1CH "
                "(setup 1EH = 80H), 7-segment 19H (setup 1FH = 80H).\n");

    /* how the run ended */
    switch (r->stop) {
    case SIM_INT3:
        if (picture) put(&o, "  The program ends with INT 3 at %04X (back to the kit's monitor).\n", r->stop_ip);
        break;
    case SIM_HLT:
        if (picture) put(&o, "  The program stops with HLT at %04X (press RESET on the kit afterwards).\n", r->stop_ip);
        break;
    case SIM_DOS:
        put(&o, "!INT 21H at %04X: the kit has no DOS, so DOS services (INT 21H) do not work there. "
                "Kit programs loop forever (JMP back) or end with INT 3.\n", r->stop_ip);
        break;
    case SIM_INT:
        put(&o, "!INT %02XH at %04X: the program did not set up this interrupt itself, so the check "
                "cannot follow it (the kit has no DOS or BIOS).\n", r->stop_int, r->stop_ip);
        break;
    case SIM_DIV0:
        put(&o, "!Divide error at %04X: DIV/IDIV by 0, or the result does not fit.\n", r->stop_ip);
        break;
    case SIM_BADOP:
        put(&o, "!Byte %02XH at %04X is not an 8086 instruction (did the program run into its data?).\n",
            r->stop_op, r->stop_ip);
        break;
    case SIM_NOT8086:
        put(&o, "!The instruction at %04X (byte %02XH) needs an 80186/286 or an 8087; the kit's 8086 "
                "cannot run it. Example: use MOV CL,4 / SHL AL,CL instead of SHL AL,4.\n",
            r->stop_ip, r->stop_op);
        break;
    case SIM_OUTSIDE:
        put(&o, "!The program ran outside its own bytes (to %04X:%04X). End it with a JMP back to "
                "the loop, or with INT 3.\n", r->stop_cs, r->stop_ip);
        break;
    case SIM_ROM:
        if (picture)
            put(&o, "  The program calls the kit's monitor ROM at %04X:%04X; the check stops there.\n",
                r->stop_cs, r->stop_ip);
        break;
    }

    /* dot matrix */
    if (r->dm_before_init)
        put(&o, "!The dot matrix ports (18H/1AH/1CH) are used before its 8255 is set up. "
                "Put MOV AL,80H / OUT 1EH,AL first.\n");
    else if (r->dm_used && r->nframes == 0)
        put(&o, "!Nothing lights up on the dot matrix. A column lights when its bit in 1CH is 1, "
                "and a row LED when its bit in 1AH is 0.\n");
    if (r->dm_mostly_on)
        put(&o, "!Almost all dot matrix LEDs are lit. On this kit 0 = LED on, 1 = off: check the "
                "bytes of your table (and DS, if the table is in .DATA).\n");

    /* 7-segment */
    if (r->seg_before_init)
        put(&o, "!The 7-segment port (19H) is used before its 8255 is set up. "
                "Put MOV AL,80H / OUT 1FH,AL first.\n");
    if (r->ndigits >= 2) {
        int cc = 0, ok = 0;
        for (i = 0; i < r->ndigits; i++) {
            if (is_digit_code(r->digit[i])) ok++;
            else if (is_digit_code((~r->digit[i]) & 0xFF)) cc++;
        }
        if (cc > ok)
            put(&o, "!These 7-segment codes look like common-cathode codes (1 = segment on). The kit "
                    "is common-anode: 0 = segment on, e.g. 0 is C0H (not 3FH).\n");
    }

    /* other ports */
    for (i = 0; i < r->nports; i++) {
        int exists;
        const char *nm = port_name(r->port[i], &exists);
        if (!exists)
            put(&o, "!Port %02XH: %s.\n", r->port[i], nm);
        else if (r->port[i] == 0x08 || r->port[i] == 0x0A)
            put(&o, "!Port %02XH is the %s; writing to it can stop the PC link until RESET.\n",
                r->port[i], nm);
        else if (picture)
            put(&o, "  Port %02XH: %s (not shown by this check).\n", r->port[i], nm);
    }
    if (!o.buf) put(&o, "%s", "");
    return o.buf;
}
