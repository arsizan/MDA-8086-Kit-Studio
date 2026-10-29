#!/usr/bin/env python3
"""A fake MDA-8086 kit (serial monitor) on a pseudo-terminal, for testing.

It behaves like the lab's kit as seen in the real kit's log:
  banner + "8086 >" after RESET, echo of typed characters, L download of Intel HEX
  ("Down load start !!" ... "OK Completed !!"), G command ("Execute Address = ..."),
  D dump. After G the "program runs": the monitor is silent until RESET.

Usage: fakekit.py STATEDIR [--silent-until-reset] [--dead-port]
  writes STATEDIR/port (pty path to give to the program under test),
  STATEDIR/memory.json after each download, STATEDIR/log.txt (everything received).
  touch STATEDIR/reset  -> simulate pressing RESET (PC mode).
  touch STATEDIR/quit   -> exit.
"""
import os, sys, json, time, tty, select

state_dir = sys.argv[1]
silent = '--silent-until-reset' in sys.argv
os.makedirs(state_dir, exist_ok=True)

master, slave = os.openpty()
tty.setraw(slave)
slave_path = os.ttyname(slave)
open(os.path.join(state_dir, 'port'), 'w').write(slave_path)

dead_master = dead_slave = None
if '--dead-port' in sys.argv:          # a second port with nothing connected
    dead_master, dead_slave = os.openpty()
    tty.setraw(dead_slave)
    open(os.path.join(state_dir, 'deadport'), 'w').write(os.ttyname(dead_slave))

memory = {}
log = open(os.path.join(state_dir, 'log.txt'), 'ab', buffering=0)
mode = 'off' if silent else 'monitor'      # off / monitor / download / running
line = ''
events = []

def send(s):
    os.write(master, s.encode('latin-1'))

def banner():
    global mode, line
    mode = 'monitor'
    line = ''
    send('\r\n** 8086 Monitor 9.5 **\r\n** Midas 2109-9964/5 **\r\n\r\n8086 >')

def save_memory():
    json.dump({f'{a:04X}': v for a, v in sorted(memory.items())},
              open(os.path.join(state_dir, 'memory.json'), 'w'))

def command(cmd):
    global mode
    c = cmd.strip()
    u = c.upper()
    events.append(c)
    if c == '':
        send('\r\n8086 >')
    elif u == 'L':
        send('\r\n\r\n   Down load start !!\r\n')
        mode = 'download'
    elif u.startswith('G'):
        arg = c[1:].strip() or '0000:1000'
        send('\r\n\r\nExecute Address = %s \r\n' % arg.upper())
        mode = 'running'
    elif u.startswith('D'):
        parts = c[1:].split()
        try:
            seg, off = parts[0].split(':')
            off = int(off, 16)
            n = int(parts[1], 16) if len(parts) > 1 else 0x80
        except Exception:
            send('\r\n   Command Error !!\a\r\n8086 >')
            return
        out = '\r\n'
        for row in range(0, n, 16):
            vals = [memory.get(off + row + i, 0xFF) for i in range(16)]
            out += '%s:%04X ' % (seg.upper(), off + row) + ' '.join('%02X' % v for v in vals[:8]) + ' - ' + \
                   ' '.join('%02X' % v for v in vals[8:]) + '\r\n'
        send(out + '8086 >')
    else:
        send('\r\n   Command Error !!\a\r\n8086 >')

def hexline(l):
    global mode
    l = l.strip()
    if not l.startswith(':'):
        return
    try:
        b = bytes.fromhex(l[1:])
    except ValueError:
        send('\r\n   Data Error !!\r\n8086 >'); mode = 'monitor'; return
    if len(b) < 5 or b[0] + 5 != len(b) or sum(b) & 0xFF:
        send('\r\n   Check Sum Error !!\r\n8086 >'); mode = 'monitor'; return
    if b[3] == 0:
        a = (b[1] << 8) | b[2]
        for i in range(b[0]):
            memory[a + i] = b[4 + i]
    elif b[3] == 1:
        save_memory()
        send('\r\n   OK Completed !!\r\n\r\n8086 >')
        mode = 'monitor'

if not silent:
    pass   # like a kit that was RESET before the program started
last_char_t = None
while True:
    for f, action in (('reset', banner), ('quit', None)):
        p = os.path.join(state_dir, f)
        if os.path.exists(p):
            os.remove(p)
            if action is None:
                sys.exit(0)
            action()
    r, _, _ = select.select([master] + ([dead_master] if dead_master else []), [], [], 0.05)
    if dead_master in r:
        os.read(dead_master, 1024)          # nothing answers on the dead port
    if master not in r:
        continue
    try:
        data = os.read(master, 1024)
    except OSError:
        time.sleep(0.05); continue
    log.write(data)
    for ch in data.decode('latin-1'):
        if mode in ('off', 'running'):
            continue                          # KIT mode or user program: no monitor
        if mode == 'monitor':
            if ch in '\r\n':
                if ch == '\r':
                    command(line); line = ''
            elif ch == '\b':
                line = line[:-1]
            else:
                line += ch
                send(ch)                      # echo
        elif mode == 'download':
            if ch in '\r\n':
                if line:
                    l = line; line = ''
                    hexline(l)
                    if mode == 'download':
                        send('\r\n')
            else:
                line += ch
                if len(line) <= 3 or not line.startswith(':00000001'):
                    send(ch)                  # the real kit echoes records
