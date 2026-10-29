/* kitport_posix.c - serial ports on Linux, used only by the test programs.
 * Port list comes from the KIT_TEST_PORTS environment variable ("/dev/pts/3,/dev/pts/5"). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "kitport.h"

struct KitPort {
    int  fd;
    char name[KP_NAMELEN];
};

int kp_list(char names[][KP_NAMELEN], int max)
{
    const char *env = getenv("KIT_TEST_PORTS");
    int count = 0;
    char buf[1024], *tok, *save = NULL;
    if (!env) return 0;
    snprintf(buf, sizeof buf, "%s", env);
    for (tok = strtok_r(buf, ",", &save); tok && count < max; tok = strtok_r(NULL, ",", &save))
        snprintf(names[count++], KP_NAMELEN, "%s", tok);
    return count;
}

KitPort *kp_open(const char *name, char *err, int errsize)
{
    struct termios t;
    KitPort *p;
    int fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (err) snprintf(err, errsize, "%s cannot be opened (%s)", name, strerror(errno));
        return NULL;
    }
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B9600);
        cfsetospeed(&t, B9600);
        tcsetattr(fd, TCSANOW, &t);
    }
    p = (KitPort *)calloc(1, sizeof *p);
    p->fd = fd;
    snprintf(p->name, sizeof p->name, "%s", name);
    return p;
}

void kp_close(KitPort *p) { if (p) { close(p->fd); free(p); } }
const char *kp_name(const KitPort *p) { return p ? p->name : ""; }

int kp_write(KitPort *p, const char *buf, int n)
{
    int done = 0;
    while (done < n) {
        ssize_t w = write(p->fd, buf + done, n - done);
        if (w < 0) {
            if (errno == EAGAIN) { usleep(1000); continue; }
            return -1;
        }
        done += (int)w;
    }
    return done;
}

int kp_read(KitPort *p, char *buf, int max)
{
    ssize_t r = read(p->fd, buf, max);
    if (r < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    return (int)r;
}

void kp_flush_input(KitPort *p) { char b[256]; while (kp_read(p, b, sizeof b) > 0) {} }

unsigned kp_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void kp_sleep(int ms) { usleep(ms * 1000); }
void kp_wait_us(int us) { usleep(us); }
