/* kitsim.h - run a kit program on a model of the MDA-8086 (8086 CPU + the two 8255s that
 * drive the dot matrix and the 7-segment display) and describe what the kit would show. */
#ifndef KITSIM_H
#define KITSIM_H

#define SIM_CLOCK      4915200.0   /* 14.7456 MHz crystal / 3 */
#define SIM_MAXFRAMES  6
#define SIM_MAXDIGITS  24
#define SIM_MAXPORTS   24

enum {
    SIM_TIMEUP,       /* still running when the time was over (normal for kit programs) */
    SIM_INT3,         /* INT 3: back to the kit's monitor */
    SIM_HLT,          /* HLT */
    SIM_DOS,          /* INT 21H (DOS) - not on the kit */
    SIM_INT,          /* other INT whose vector the program did not set */
    SIM_BADOP,        /* byte that is not an 8086 instruction */
    SIM_NOT8086,      /* 80186+ / 8087 instruction */
    SIM_OUTSIDE,      /* ran outside the program's bytes */
    SIM_ROM,          /* jumped into the monitor ROM (F0000H-FFFFFH) */
    SIM_DIV0          /* divide error without own handler */
};

typedef struct {
    int stop;                     /* SIM_... */
    unsigned stop_cs, stop_ip;    /* where it stopped */
    int stop_int;                 /* INT number (SIM_INT, SIM_DOS) */
    int stop_op;                  /* opcode (SIM_BADOP, SIM_NOT8086) */
    double seconds;               /* simulated kit time */
    unsigned long insns;          /* instructions executed */

    /* dot matrix (8255 CS1: 18H red rows, 1AH green rows, 1CH columns, 1EH control) */
    int dm_used, dm_init, dm_before_init, dm_red;
    int nframes, frames_more, nchanges;
    char frame[SIM_MAXFRAMES][8][9];      /* '#' green, 'R' red, '@' both, '.' off */
    double frame_t[SIM_MAXFRAMES];
    int dm_mostly_on;                     /* most LEDs lit in most pictures */

    /* 7-segment (8255 CS2: 19H segments, 1BH/1DH, 1FH control) */
    int seg_used, seg_init, seg_before_init;
    int ndigits, digits_more;
    unsigned char digit[SIM_MAXDIGITS];   /* values written to 19H that stayed >= 20 ms */
    double digit_t[SIM_MAXDIGITS];

    /* every other port the program used */
    int nports;
    unsigned short port[SIM_MAXPORTS];
    unsigned char port_dir[SIM_MAXPORTS]; /* bit 0 = OUT, bit 1 = IN */
} KitSimReport;

/* Run 'code' loaded at 0000:'base' (registers as after RESET: segments 0, SP 0540H,
 * IP = base) for at most 'seconds' of kit time. Never touches anything outside. */
void kit_simulate(const unsigned char *code, int len, unsigned base, double seconds,
                  KitSimReport *r);

/* Text for the output pane, '\n' separated. Lines that start with '!' are warnings.
 * picture = 1: include the dot-matrix pictures and 7-segment digits. malloc'd. */
char *kit_sim_text(const KitSimReport *r, int picture);

/* ---- the Simulator panel: the same model, run step by step or in real time ---- */
typedef struct KitMachine KitMachine;

typedef struct {
    unsigned short ax, bx, cx, dx, si, di, bp, sp, cs, ds, es, ss, ip, flags;
    unsigned long long cycles;            /* kit clock cycles since reset (4.9152 MHz) */
    unsigned long insns;
    int stop, stop_int, stop_op;          /* stop: -1 = can run, else SIM_... */
    unsigned char p1[3], p2[3];           /* 8255 #1 (dot matrix) and #2 (LEDs, 7-seg): A, B, C */
    unsigned char p1cr, p2cr;             /* control words (0 = not set up) */
    int p1_set, p2_set;
    unsigned char dm_red[64], dm_green[64];   /* brightness 0..255, index row*8+col (0,0 = top left) */
    unsigned char seg[8];                     /* a b c d e f g dp */
    unsigned char led[4];                     /* PB0..PB3 */
    char lcd[2][17];
    int lcd_on;
} KitView;

KitMachine *km_new(const unsigned char *code, int len, unsigned base);
void km_free(KitMachine *k);
void km_reset(KitMachine *k);                              /* back to the start, as after RESET */
int  km_run(KitMachine *k, unsigned long long cycles);     /* returns -1 or the SIM_... stop */
int  km_step(KitMachine *k);                               /* one instruction */
/* registers, ports and what the displays showed since the last call */
void km_view(KitMachine *k, KitView *v);

#endif
