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

#endif
