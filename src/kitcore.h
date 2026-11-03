/* kitcore.h - talk to the MDA-8086 serial monitor ("8086 >" prompt) */
#ifndef KITCORE_H
#define KITCORE_H
#include <stdio.h>
#include "kitport.h"

enum { KO_INFO, KO_OK, KO_WARN, KO_ERR, KO_KIT };

typedef void (*KitOutFn)(void *ctx, int kind, const char *text);

typedef struct {
    KitOutFn       out;
    void          *ctx;
    volatile long *cancel;          /* set to 1 to stop a long wait */
    char           want[KP_NAMELEN];/* port to use; "" = find automatically */
    int            wait_reset_ms;   /* how long to wait for RESET (default 60000) */
    KitPort       *port;            /* the kit's port while connected */
    int            cr_pending;      /* newline handling between reads */
    FILE          *log;             /* raw serial log (bytes in hex), or NULL */
    unsigned       log_t0;
    long           log_size;
} KitLink;

void kit_init(KitLink *k, KitOutFn out, void *ctx, volatile long *cancel);
/* write every byte sent/received and every message to f (opened by the caller) */
void kit_set_log(KitLink *k, FILE *f);
/* make sure k->port is a kit that answers; may ask the user to press RESET */
int  kit_find(KitLink *k, int allow_wait);
/* L command + Intel HEX records from 'abs' text; 1 = kit said "Completed" */
int  kit_download(KitLink *k, const char *abs, int *nbytes);
/* G seg:off; 1 = kit said "Execute" */
int  kit_go(KitLink *k, unsigned seg, unsigned off);
/* send one monitor command line (adds CR) */
int  kit_command(KitLink *k, const char *line);
/* show anything the kit sent; returns -1 if the port stopped working */
int  kit_poll(KitLink *k);
void kit_disconnect(KitLink *k);

/* checks an .ABS text: returns number of data records (0 = none), -1 if damaged */
int  kit_check_abs(const char *abs, int *nbytes, char *why, int whysize);

#endif
