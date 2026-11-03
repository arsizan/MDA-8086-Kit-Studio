/* kitport_posix.c - serial ports on Linux (the Linux program and the test programs).
 * Ports: /dev/ttyS* that have a UART behind them (built-in COM ports), /dev/ttyUSB*
 * (USB-to-RS232 adapters: FTDI, CH340, PL2303, ...) and /dev/ttyACM*.
 * For tests, KIT_TEST_PORTS="/dev/pts/3,/dev/pts/5" replaces the list. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <grp.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "kitport.h"

struct KitPort {
    int  fd;
    char name[KP_NAMELEN];
};

/* ------------------------------------------------------------------ list */
/* /dev/ttyS0..31 exist on most PCs even without a COM port: keep only real UARTs */
static int real_ttyS(const char *dev)
{
    char path[160], buf[32] = "";
    FILE *f;
    snprintf(path, sizeof path, "/sys/class/tty/%.40s/type", dev);
    f = fopen(path, "r");
    if (!f) return 1;                       /* cannot tell: keep it */
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    return atoi(buf) != 0;                  /* 0 = no UART found at this port */
}

static int port_kind(const char *dev, int *num)
{
    static const char *const pre[] = { "ttyS", "ttyUSB", "ttyACM", NULL };
    int i;
    for (i = 0; pre[i]; i++) {
        size_t n = strlen(pre[i]);
        if (strncmp(dev, pre[i], n) == 0 && dev[n] >= '0' && dev[n] <= '9') {
            *num = atoi(dev + n);
            return i;
        }
    }
    return -1;
}

typedef struct { int kind, num; char name[KP_NAMELEN]; } PortEntry;

static int cmp_entry(const void *a, const void *b)
{
    const PortEntry *x = (const PortEntry *)a, *y = (const PortEntry *)b;
    if (x->kind != y->kind) return x->kind - y->kind;
    return x->num - y->num;
}

int kp_list(char names[][KP_NAMELEN], int max)
{
    const char *env = getenv("KIT_TEST_PORTS");
    PortEntry e[KP_MAXPORTS];
    struct dirent *de;
    DIR *d;
    int n = 0, i;
    if (env) {
        char buf[1024], *tok, *save = NULL;
        snprintf(buf, sizeof buf, "%s", env);
        for (tok = strtok_r(buf, ",", &save); tok && n < max; tok = strtok_r(NULL, ",", &save))
            snprintf(names[n++], KP_NAMELEN, "%s", tok);
        return n;
    }
    d = opendir("/dev");
    if (!d) return 0;
    while ((de = readdir(d)) != NULL && n < KP_MAXPORTS) {
        int num, kind = port_kind(de->d_name, &num);
        if (kind < 0 || strlen(de->d_name) > 40) continue;
        if (kind == 0 && !real_ttyS(de->d_name)) continue;
        e[n].kind = kind;
        e[n].num = num;
        snprintf(e[n].name, KP_NAMELEN, "/dev/%.40s", de->d_name);
        n++;
    }
    closedir(d);
    qsort(e, n, sizeof e[0], cmp_entry);
    if (n > max) n = max;
    for (i = 0; i < n; i++) snprintf(names[i], KP_NAMELEN, "%s", e[i].name);
    return n;
}

/* ------------------------------------------------------------------ open / close */
KitPort *kp_open(const char *name, char *err, int errsize)
{
    struct termios t;
    KitPort *p;
    int bits, fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        int e = errno;
        if (err) {
            if (e == EACCES || e == EPERM) {
                char grp[64] = "dialout";
                struct stat st;
                if (stat(name, &st) == 0) {
                    struct group *g = getgrgid(st.st_gid);
                    if (g && g->gr_name && strcmp(g->gr_name, "root") != 0)
                        snprintf(grp, sizeof grp, "%s", g->gr_name);
                }
                snprintf(err, errsize, "%s: no permission. Run once in a terminal: "
                         "sudo usermod -aG %s $USER   then log out and log in again.", name, grp);
            } else if (e == EBUSY) {
                snprintf(err, errsize, "%s is busy (another program is using it)", name);
            } else {
                snprintf(err, errsize, "%s cannot be opened (%s)", name, strerror(e));
            }
        }
        return NULL;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        close(fd);
        if (err) snprintf(err, errsize, "%s is busy (another program is using it)", name);
        return NULL;
    }
    if (tcgetattr(fd, &t) != 0) {
        close(fd);
        if (err) snprintf(err, errsize, "%s is not a working serial port (%s)", name, strerror(errno));
        return NULL;
    }
    cfmakeraw(&t);                          /* 8 data bits, no parity, raw bytes */
    t.c_cflag &= ~(CSTOPB | CRTSCTS);       /* 1 stop bit, no hardware handshake */
    t.c_cflag |= CLOCAL | CREAD;
    t.c_iflag &= ~(IXON | IXOFF | IXANY);   /* no XON/XOFF */
    t.c_cc[VMIN] = 1;                       /* with O_NONBLOCK: no data = EAGAIN, 0 = hang-up */
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, B9600);
    cfsetospeed(&t, B9600);
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        close(fd);
        if (err) snprintf(err, errsize, "%s: cannot set 9600 baud (%s)", name, strerror(errno));
        return NULL;
    }
    ioctl(fd, TIOCEXCL);                    /* no other program may open it while we use it */
    bits = TIOCM_DTR | TIOCM_RTS;           /* DTR and RTS on, as on Windows */
    ioctl(fd, TIOCMBIS, &bits);
    tcflush(fd, TCIOFLUSH);
    p = (KitPort *)calloc(1, sizeof *p);
    if (!p) { close(fd); return NULL; }
    p->fd = fd;
    snprintf(p->name, sizeof p->name, "%s", name);
    return p;
}

void kp_close(KitPort *p)
{
    if (!p) return;
    ioctl(p->fd, TIOCNXCL);
    flock(p->fd, LOCK_UN);
    close(p->fd);
    free(p);
}

const char *kp_name(const KitPort *p) { return p ? p->name : ""; }

/* ------------------------------------------------------------------ read / write */
int kp_write(KitPort *p, const char *buf, int n)
{
    int done = 0;
    unsigned t0 = kp_ms();
    while (done < n) {
        ssize_t w = write(p->fd, buf + done, n - done);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                if ((int)(kp_ms() - t0) > 3000) return -1;    /* stuck: adapter gone */
                kp_sleep(1);
                continue;
            }
            return -1;
        }
        done += (int)w;
    }
    return done;
}

int kp_read(KitPort *p, char *buf, int max)
{
    ssize_t r = read(p->fd, buf, max);
    if (r > 0) return (int)r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;                              /* 0 = hang-up (adapter unplugged) or an error */
}

void kp_flush_input(KitPort *p)
{
    char b[256];
    tcflush(p->fd, TCIFLUSH);
    while (kp_read(p, b, sizeof b) > 0) {}
}

/* ------------------------------------------------------------------ time */
unsigned kp_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void kp_sleep(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void kp_wait_us(int us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (long)(us % 1000000) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}
