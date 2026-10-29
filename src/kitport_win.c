/* kitport_win.c - serial ports on Windows (XP ... 11, 32 and 64 bit).
 * Works with built-in COM ports and USB-to-RS232 adapters (they also appear as COMx). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kitport.h"

struct KitPort {
    HANDLE h;
    char   name[KP_NAMELEN];
};

static int com_number(const char *s)
{
    int n = 0;
    if (_strnicmp(s, "COM", 3) != 0) return -1;
    s += 3;
    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return *s ? -1 : n;
}

static void add_port(char names[][KP_NAMELEN], int *count, int max, const char *name)
{
    int i, n = com_number(name);
    if (n <= 0 || *count >= max) return;
    for (i = 0; i < *count; i++)
        if (com_number(names[i]) == n) return;
    snprintf(names[*count], KP_NAMELEN, "COM%d", n);
    (*count)++;
}

int kp_list(char names[][KP_NAMELEN], int max)
{
    int count = 0, i, j;
    HKEY key;
    /* 1. ports Windows has drivers for (includes USB adapters) */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        for (i = 0; ; i++) {
            char vname[256], data[64];
            DWORD vlen = sizeof vname, dlen = sizeof data - 1, type;
            if (RegEnumValueA(key, i, vname, &vlen, NULL, &type, (BYTE *)data, &dlen) != ERROR_SUCCESS)
                break;
            if (type == REG_SZ) {
                data[dlen] = 0;
                add_port(names, &count, max, data);
            }
        }
        RegCloseKey(key);
    }
    /* 2. every COMx name in the DOS device list (catches anything the registry missed) */
    {
        DWORD size = 65536;
        char *buf = (char *)malloc(size);
        if (buf) {
            DWORD got = QueryDosDeviceA(NULL, buf, size);
            if (got) {
                char *p = buf;
                while (*p && p < buf + got) {
                    add_port(names, &count, max, p);
                    p += strlen(p) + 1;
                }
            }
            free(buf);
        }
    }
    /* sort by number */
    for (i = 0; i < count; i++)
        for (j = i + 1; j < count; j++)
            if (com_number(names[j]) < com_number(names[i])) {
                char t[KP_NAMELEN];
                memcpy(t, names[i], KP_NAMELEN);
                memcpy(names[i], names[j], KP_NAMELEN);
                memcpy(names[j], t, KP_NAMELEN);
            }
    return count;
}

KitPort *kp_open(const char *name, char *err, int errsize)
{
    char path[KP_NAMELEN + 8];
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    KitPort *p;

    snprintf(path, sizeof path, "\\\\.\\%s", name);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (err) {
            if (e == ERROR_ACCESS_DENIED)
                snprintf(err, errsize, "%s is busy (another program, e.g. PuTTY, is using it)", name);
            else if (e == ERROR_FILE_NOT_FOUND)
                snprintf(err, errsize, "%s is not present", name);
            else
                snprintf(err, errsize, "%s cannot be opened (Windows error %lu)", name, (unsigned long)e);
        }
        return NULL;
    }
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = CBR_9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        if (err) snprintf(err, errsize, "%s does not accept 9600 baud settings", name);
        CloseHandle(h);
        return NULL;
    }
    SetupComm(h, 4096, 4096);
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;      /* ReadFile returns at once with what is there */
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 0;
    to.WriteTotalTimeoutMultiplier = 2;
    to.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(h, &to);
    EscapeCommFunction(h, SETDTR);
    EscapeCommFunction(h, SETRTS);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    p = (KitPort *)calloc(1, sizeof *p);
    if (!p) { CloseHandle(h); return NULL; }
    p->h = h;
    snprintf(p->name, sizeof p->name, "%s", name);
    return p;
}

void kp_close(KitPort *p)
{
    if (!p) return;
    CloseHandle(p->h);
    free(p);
}

const char *kp_name(const KitPort *p) { return p ? p->name : ""; }

int kp_write(KitPort *p, const char *buf, int n)
{
    DWORD done = 0;
    if (!WriteFile(p->h, buf, (DWORD)n, &done, NULL)) {
        DWORD errs;
        ClearCommError(p->h, &errs, NULL);
        return -1;
    }
    return (int)done;
}

int kp_read(KitPort *p, char *buf, int max)
{
    DWORD got = 0;
    if (!ReadFile(p->h, buf, (DWORD)max, &got, NULL)) {
        DWORD errs;
        DWORD e = GetLastError();
        if (ClearCommError(p->h, &errs, NULL) && e != ERROR_ACCESS_DENIED &&
            e != ERROR_BAD_COMMAND && e != ERROR_DEVICE_NOT_CONNECTED &&
            e != ERROR_GEN_FAILURE && e != ERROR_INVALID_HANDLE)
            return 0;                       /* line error (framing, overrun): ignore */
        return -1;                          /* adapter unplugged or port gone */
    }
    return (int)got;
}

void kp_flush_input(KitPort *p)
{
    PurgeComm(p->h, PURGE_RXCLEAR);
}

unsigned kp_ms(void) { return (unsigned)GetTickCount(); }

void kp_sleep(int ms) { Sleep(ms); }

void kp_wait_us(int us)
{
    LARGE_INTEGER f, a, b;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) {
        Sleep(us >= 1000 ? us / 1000 : 1);
        return;
    }
    QueryPerformanceCounter(&a);
    do {
        QueryPerformanceCounter(&b);
    } while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < us);
}
