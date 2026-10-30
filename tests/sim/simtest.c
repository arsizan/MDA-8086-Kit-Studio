/* simtest FILE.ABS [seconds] : run an .ABS on the kit model and print the report */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/kitsim.h"
#include "../../src/kitasm.h"
static char *slurp(const char *p) { FILE *f = fopen(p, "rb"); long n; char *b; if (!f) return NULL; fseek(f, 0, 2); n = ftell(f); fseek(f, 0, 0); b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0; fclose(f); return b; }
int main(int argc, char **argv) {
    int i;
    double secs = 12.0;
    if (argc > 2 && argv[argc-1][0] >= '0' && argv[argc-1][0] <= '9') { secs = atof(argv[argc-1]); argc--; }
    for (i = 1; i < argc; i++) {
        char *t = slurp(argv[i]), why[200], *txt;
        unsigned char *code; int len; unsigned base;
        KitSimReport r;
        if (!t || kit_parse_abs(t, &code, &len, &base, why, sizeof why)) { printf("%s: cannot read\n", argv[i]); continue; }
        kit_simulate(code, len, base, secs, &r);
        txt = kit_sim_text(&r, 1);
        printf("=== %s  (stop=%d insns=%lu %.2fs)\n%s", argv[i], r.stop, r.insns, r.seconds, txt);
        free(txt); free(code); free(t);
    }
    return 0;
}
