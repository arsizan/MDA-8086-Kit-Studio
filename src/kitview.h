/* kitview.h - the Simulator panel's picture (LEDs, 7-segment, dot matrix, LCD, registers,
 * 8255 ports), drawn the same way by the Windows (GDI+) and the Linux (cairo) window.
 * The window gives a few drawing functions; kitview_draw lays everything out. */
#ifndef KITVIEW_H
#define KITVIEW_H
#include "kitsim.h"

#define KV_NONE 0xFF000000u                 /* "no fill" / "no outline" colour */
enum { KV_BOLD = 1, KV_MONO = 2, KV_CENTER = 4, KV_RIGHT = 8 };

typedef struct {
    void *ctx;
    /* colours are 0xRRGGBB; coordinates in pixels of the panel */
    void (*rect)(void *ctx, double x, double y, double w, double h, double radius, unsigned fill, unsigned line);
    void (*circle)(void *ctx, double cx, double cy, double r, unsigned fill, unsigned line, double width);
    void (*poly)(void *ctx, const double *xy, int n, unsigned fill);
    /* y = top of the text; size = font height in pixels */
    void (*text)(void *ctx, double x, double y, const char *s, double size, int flags, unsigned color);
} KitDraw;

typedef struct {
    KitView v;              /* registers and displays (km_view) */
    int have;               /* a program is loaded */
    int running;            /* running in real time */
    double speed;           /* 1.0 = the real kit's speed */
    int line;               /* source line at CS:IP, 0 = not known */
    char line_text[100];    /* that line */
} KitPanel;

/* draw the whole panel into w x h pixels */
void kitview_draw(const KitDraw *d, double w, double h, const KitPanel *p);

/* "1x (real kit)", "1/100x (slow motion)" ... */
void kitview_speed_text(double speed, char *buf, int size);

#endif
