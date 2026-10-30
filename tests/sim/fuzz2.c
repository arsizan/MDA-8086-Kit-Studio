/* structured random kit programs that loop forever: MOV AL,x / OUT p,AL / delay loops / JMP start */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/kitsim.h"
int main(int argc, char **argv)
{
    unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    int n = argc > 2 ? atoi(argv[2]) : 200, i, k;
    unsigned char c[2048];
    clock_t t0 = clock();
    double worst = 0;
    srand(seed);
    for (i = 0; i < n; i++) {
        KitSimReport r;
        char *t;
        int len = 0, parts = 1 + rand() % 40;
        clock_t a = clock();
        double el;
        for (k = 0; k < parts && len < 2000; k++) {
            int kind = rand() % 6;
            if (kind <= 2) {
                c[len++] = 0xB0; c[len++] = (unsigned char)(rand() % 3 == 0 ? 0x80 : rand());
                c[len++] = 0xE6; c[len++] = (unsigned char)(0x18 + rand() % 8);
            } else if (kind == 3) {
                c[len++] = 0xB9; c[len++] = (unsigned char)rand(); c[len++] = (unsigned char)(rand() % 8);
                c[len++] = 0xE2; c[len++] = 0xFE;           /* LOOP $ */
            } else if (kind == 4) {
                c[len++] = 0xD0; c[len++] = 0xC4;           /* ROL AH,1 */
                c[len++] = 0x8A; c[len++] = 0xC4;           /* MOV AL,AH */
                c[len++] = 0xE6; c[len++] = 0x1C;
            } else {
                c[len++] = 0x90;
            }
        }
        { int back = -(len + 3); c[len++] = 0xE9; c[len++] = (unsigned char)(back & 0xFF); c[len++] = (unsigned char)((back >> 8) & 0xFF); }
        kit_simulate(c, len, 0x1000, 12.0, &r);
        t = kit_sim_text(&r, 1);
        if (i < 2) printf("%s", t);
        free(t);
        el = (double)(clock() - a) / CLOCKS_PER_SEC;
        if (el > worst) worst = el;
        if (r.stop != SIM_TIMEUP) printf("unexpected stop %d\n", r.stop);
    }
    printf("fuzz2 seed=%u programs=%d total=%.1fs worst=%.3fs\n", seed, n, (double)(clock() - t0) / CLOCKS_PER_SEC, worst);
    return 0;
}
