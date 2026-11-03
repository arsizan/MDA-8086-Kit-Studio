/* kittest: drive kitcore against a (fake) kit.  kittest FILE.ABS [nowait] [cancel_after_ms]
   Ports come from KIT_TEST_PORTS (POSIX build) or the system (Windows build).
   KIT_TEST_LOG=file  -> raw serial log;  KIT_TEST_DIRECT=1 -> G right after the download
   (as the program's Run button does), without the D command in between. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/kitcore.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
static const char *K[] = { "INFO", "OK", "WARN", "ERR", "KIT" };
static void out(void *ctx, int kind, const char *t) { (void)ctx; printf("[%s] %s%s", K[kind], t, kind == KO_KIT ? "" : "\n"); fflush(stdout); }
static volatile long cancel_flag = 0;
static int cancel_after = 0;
#ifdef _WIN32
static DWORD WINAPI canceller(LPVOID a) { (void)a; Sleep(cancel_after); cancel_flag = 1; return 0; }
#else
static void *canceller(void *a) { (void)a; usleep(cancel_after * 1000); cancel_flag = 1; return NULL; }
#endif
int main(int argc, char **argv)
{
    KitLink k;
    FILE *f; char *abs; long n; int bytes = 0, ok;
    if (argc < 2) return 2;
    f = fopen(argv[1], "rb"); if (!f) { printf("no file\n"); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    abs = malloc(n + 1); fread(abs, 1, n, f); abs[n] = 0; fclose(f);
    if (argc > 3) {
        cancel_after = atoi(argv[3]);
#ifdef _WIN32
        CreateThread(NULL, 0, canceller, NULL, 0, NULL);
#else
        { pthread_t t; pthread_create(&t, NULL, canceller, NULL); }
#endif
    }
    kit_init(&k, out, NULL, &cancel_flag);
    if (getenv("KIT_TEST_LOG")) {
        FILE *lf = fopen(getenv("KIT_TEST_LOG"), "a");
        if (lf) kit_set_log(&k, lf);
    }
    if (getenv("KIT_PORT")) snprintf(k.want, sizeof k.want, "%s", getenv("KIT_PORT"));
    k.wait_reset_ms = 15000;
    ok = kit_find(&k, !(argc > 2 && strcmp(argv[2], "nowait") == 0));
    printf("\n== find=%d\n", ok);
    if (!ok) return 1;
    ok = kit_download(&k, abs, &bytes);
    printf("\n== download=%d bytes=%d\n", ok, bytes);
    if (!ok) return 1;
    if (!getenv("KIT_TEST_DIRECT")) {
        kit_command(&k, "D 0000:1000 30");
        { unsigned t0 = kp_ms(); while (kp_ms() - t0 < 800) { kit_poll(&k); kp_sleep(20); } }
    }
    ok = kit_go(&k, 0, 0x1000);
    printf("\n== go=%d\n", ok);
    kit_disconnect(&k);
    return ok ? 0 : 1;
}
