#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/kitsim.h"
int main(int argc, char **argv)
{
    unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    int n = argc > 2 ? atoi(argv[2]) : 2000, i, k;
    unsigned char code[4096];
    unsigned long total = 0;
    int stops[16] = {0};
    clock_t t0 = clock();
    srand(seed);
    for (i = 0; i < n; i++) {
        KitSimReport r;
        char *t;
        int len = 1 + rand() % 4096;
        for (k = 0; k < len; k++) code[k] = (unsigned char)rand();
        if (i % 3 == 0) {   /* bias towards OUT to the kit ports and loops */
            for (k = 0; k + 2 < len; k += 7) { code[k] = 0xE6; code[k + 1] = 0x18 + rand() % 8; }
        }
        kit_simulate(code, len, 0x1000, 0.5, &r);
        t = kit_sim_text(&r, 1);
        if (!t) { printf("NULL text\n"); return 1; }
        free(t);
        t = kit_sim_text(&r, 0);
        free(t);
        total += r.insns;
        stops[r.stop & 15]++;
    }
    printf("fuzz seed=%u programs=%d insns=%lu time=%.1fs stops:", seed, n, total, (double)(clock() - t0) / CLOCKS_PER_SEC);
    for (k = 0; k < 10; k++) printf(" %d", stops[k]);
    printf("\n");
    return 0;
}
