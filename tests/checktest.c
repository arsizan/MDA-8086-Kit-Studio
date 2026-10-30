/* checktest FILE.ASM... : what the app prints after Assemble (messages, hints, kit check) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/kitasm.h"
#include "../src/kitsim.h"
#include "../src/kithint.h"
static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb"); long n; char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = (char *)malloc(n + 1);
    n = (long)fread(b, 1, n, f); b[n] = 0; fclose(f);
    return b;
}
int main(int argc, char **argv)
{
    int i;
    for (i = 2; i < argc; i++) {
        KitAsmResult r;
        char *src = slurp(argv[i]);
        printf("==================== %s\n", argv[i]);
        kit_assemble(argv[i], argv[1], &r);
        if (r.messages) printf("%s", r.messages);
        if (r.errors) printf("%d error(s).\n", r.errors);
        if (r.messages) { char *h = kit_hints(r.messages, src); if (h) { printf("%s", h); free(h); } }
        if (!r.ok) { if (!r.errors) printf("PROBLEM: %s\n", r.problem); }
        else {
            KitSimReport rep;
            char *t;
            printf("OK: %d bytes\n", r.len);
            kit_simulate(r.code, r.len, KIT_ORG, 12.0, &rep);
            t = kit_sim_text(&rep, 1);
            printf("%s", t);
            free(t);
        }
        kit_asm_free(&r);
        free(src);
    }
    return 0;
}
