#!/usr/bin/env python3
"""End-to-end GUI test of KitStudio.exe under Wine + Xvfb (:91) with the fake kit on COM1.
usage: gui_test.py EXE OUTDIR      (window appears at 4,30 with the default size)"""
import os, sys, subprocess, time, json, tempfile, shutil
from PIL import Image
exe, outdir = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(outdir, exist_ok=True)
env = dict(os.environ, DISPLAY=':91', WINEDEBUG='-all')
S = '/tmp/claude-0/-home-claude/911d5bc8-7aa7-5213-9528-911b2ede0c6f/scratchpad'

def sh(*a):
    return subprocess.run(list(a), env=env, capture_output=True, text=True)
def shot(name, crop_out=True):
    p = os.path.join(outdir, name + '.png')
    sh('import', '-window', 'root', p)
    if crop_out:
        Image.open(p).crop((4, 425, 936, 680)).save(os.path.join(outdir, name + '_out.png'))
def key(*keys, delay=0.3):
    for k in keys:
        sh('xdotool', 'key', k); time.sleep(delay)
def click(x, y, n=1):
    sh('xdotool', 'mousemove', str(x), str(y)); time.sleep(0.15)
    sh('xdotool', 'click', '--repeat', str(n), '--delay', '80', '1'); time.sleep(0.4)
def typetext(t):
    sh('xdotool', 'type', '--delay', '30', t)
def abs_bytes(path):
    mem = {}
    for l in open(path).read().split('\n'):
        l = l.strip().strip('\x1a')
        if l.startswith(':'):
            b = bytes.fromhex(l[1:])
            if b[3] == 0:
                a = (b[1] << 8) | b[2]
                for i in range(b[0]): mem[a + i] = b[4 + i]
    return mem
def kit_memory(d):
    p = os.path.join(d, 'memory.json')
    return {int(a, 16): v for a, v in json.load(open(p)).items()} if os.path.exists(p) else {}
results = []
def check(name, cond):
    results.append(cond); print(('PASS ' if cond else 'FAIL ') + name, flush=True)
def reset_kit(d):
    open(os.path.join(d, 'reset'), 'w').close()

d = tempfile.mkdtemp(prefix='fkgui_')
fk = subprocess.Popen([sys.executable, os.path.join(here, 'fakekit.py'), d])
while not os.path.exists(os.path.join(d, 'port')): time.sleep(0.05)
pty = open(os.path.join(d, 'port')).read()
sh('wineserver', '-k'); time.sleep(0.5)
sh('wine', 'reg', 'add', r'HKLM\Software\Wine\Ports', '/v', 'COM1', '/d', pty, '/f')
sh('wineserver', '-k'); time.sleep(1)
app = subprocess.Popen(['wine', exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(7)
wid = sh('xdotool', 'search', '--name', 'Kit Studio').stdout.split()
check('window opened', bool(wid))
sh('xdotool', 'windowfocus', wid[0]); time.sleep(0.4)
shot('01_start')

# 1. F7 then F5 with the 7-segment example (kit ready)
key('F7'); time.sleep(1.5); shot('02_assemble')
key('F5'); time.sleep(6); shot('03_run_k28')
check('7-segment program arrived in the kit exactly', kit_memory(d) == abs_bytes(f'{S}/mda/build/K28.ABS'))

# 2. Examples > "Dot matrix: A scrolls left to right", F5 while K28 runs -> wait, RESET
click(116, 40); click(165, 92); time.sleep(0.5)
key('F5'); time.sleep(3); shot('04_waiting')
reset_kit(d); time.sleep(6); shot('05_k27_after_reset')
check('A-scroll program arrived after RESET', kit_memory(d) == abs_bytes(f'{S}/mda/k27/K27.ABS'))

# 3. kit command while the program runs -> wait -> RESET -> dump of the loaded program
click(300, 635); typetext('d 0000:1000 20'); key('Return'); time.sleep(2.5)
reset_kit(d); time.sleep(5); shot('06_command_dump')

# 4. assembler error: bad line after line 6, F7
click(400, 250); key('ctrl+Home'); [key('Down', delay=0.1) for _ in range(5)]; key('End', 'Return')
typetext('     MOVX AL, 5'); key('F7'); time.sleep(1.5); shot('07_error')

# 5. Stop while waiting: fix line, run (kit at prompt), run again (kit busy), Esc
key('ctrl+z'); time.sleep(0.3); key('F5'); time.sleep(6); key('F5'); time.sleep(3); key('Escape'); time.sleep(2)
shot('08_stopped')

open(os.path.join(d, 'quit'), 'w').close()
app.terminate(); sh('wineserver', '-k'); fk.wait(timeout=5)
shutil.rmtree(d, ignore_errors=True)
print('RESULT:', 'ALL PASS' if all(results) else 'FAILURES')
