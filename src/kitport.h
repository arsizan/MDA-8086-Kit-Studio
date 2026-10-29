/* kitport.h - minimal serial-port layer (Win32 for the app, POSIX for tests) */
#ifndef KITPORT_H
#define KITPORT_H

#define KP_MAXPORTS 64
#define KP_NAMELEN  64

typedef struct KitPort KitPort;

/* names of the serial ports on this computer ("COM1", "COM3", ...), sorted */
int       kp_list(char names[][KP_NAMELEN], int max);
/* open at 9600 baud, 8 data bits, no parity, 1 stop bit, DTR and RTS on */
KitPort  *kp_open(const char *name, char *err, int errsize);
void      kp_close(KitPort *p);
const char *kp_name(const KitPort *p);
int       kp_write(KitPort *p, const char *buf, int n);   /* n, or -1 if the port failed */
int       kp_read(KitPort *p, char *buf, int max);        /* never waits; 0..max, or -1 */
void      kp_flush_input(KitPort *p);

unsigned  kp_ms(void);          /* milliseconds, steady clock */
void      kp_sleep(int ms);
void      kp_wait_us(int us);   /* short precise wait */

#endif
