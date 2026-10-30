#!/usr/bin/env python3
"""End-to-end GUI test of KitStudio.exe 1.1 under Wine + Xvfb (:91) with the fake kit on COM1.
usage: gui_test.py APPDIR OUTDIR   (APPDIR holds KitStudio.exe and examples/; window at 4,30)"""
import os, sys, subprocess, time, json, tempfile, shutil
from PIL import Image
appdir, outdir = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(outdir, exist_ok=True)
env = dict(os.environ, DISPLAY=':91', WINEDEBUG='-all')
S = '/tmp/claude-0/-home-claude/911d5bc8-7aa7-5213-9528-911b2ede0c6f/scratchpad'
EX_Y = {'JOY_SCROLL': 60, 'K27_A_DOTMATRIX': 76, 'K27_A_SCROLL': 92, 'K28_7SEGMENT': 108, 'SIZAN_SCROLL': 124}

def sh(*a):
    return subprocess.run(list(a), env=env, capture_output=True, text=True)
def shot(name):
    p = os.path.join(outdir, name + '.png')
    sh('import', '-window', 'root', p)
    return Image.open(p)
def key(*keys, delay=0.3):
    for k in keys:
        sh('xdotool', 'key', k); time.sleep(delay)
def click(x, y, n=1):
    sh('xdotool', 'mousemove', str(x), str(y)); time.sleep(0.15)
    sh('xdotool', 'click', '--repeat', str(n), '--delay', '80', '1'); time.sleep(0.4)
def typetext(t):
    sh('xdotool', 'type', '--delay', '30', t)
def example(name):
    click(116, 40); click(160, EX_Y[name]); time.sleep(0.5)
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
def editor_blank(img):
    """no dark text pixels in the editor area"""
    ed = img.crop((14, 92, 905, 400)).convert('L')
    return min(ed.get_flattened_data() if hasattr(ed, 'get_flattened_data') else ed.getdata()) > 200
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
app = subprocess.Popen(['wine', os.path.join(appdir, 'KitStudio.exe')], cwd=appdir, env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(7)
wid = sh('xdotool', 'search', '--name', 'Kit Studio').stdout.split()
check('window opened', bool(wid))
sh('xdotool', 'windowfocus', wid[0]); time.sleep(0.4)
check('editor is empty at start', editor_blank(shot('01_start')))

# Examples menu lists the folder; 7-segment example: F7 (kit check) then F5
click(116, 40); shot('02_examples_menu'); key('Escape'); key('Escape')
example('K28_7SEGMENT')
key('F7'); time.sleep(2.5); shot('03_assemble_k28')
# File > New gives an empty editor (the example copy was not changed, so no question)
key('ctrl+n'); time.sleep(1.5)
check('File > New gives an empty editor', editor_blank(shot('03b_new')))
example('K28_7SEGMENT')
key('F5'); time.sleep(7); shot('04_run_k28')
check('7-segment program arrived in the kit exactly', kit_memory(d) == abs_bytes(f'{S}/mda/build/K28.ABS'))

# A-scroll example while K28 runs -> waits for RESET
example('K27_A_SCROLL')
key('F5'); time.sleep(4); shot('05_waiting')
reset_kit(d); time.sleep(7); shot('06_k27_after_reset')
check('A-scroll program arrived after RESET', kit_memory(d) == abs_bytes(f'{S}/mda/k27/K27.ABS'))

# kit command while the program runs -> wait -> RESET -> memory dump
click(300, 635); typetext('d 0000:1000 20'); key('Return'); time.sleep(3)
reset_kit(d); time.sleep(5); shot('07_command_dump')

# assembler error with a hint: SHL AL,4 after line 6
click(400, 250); key('ctrl+Home'); [key('Down', delay=0.1) for _ in range(5)]; key('End', 'Return')
typetext('     SHL AL, 4'); key('F7'); time.sleep(2); shot('08_error_hint')

# Stop while waiting: undo, run (kit at prompt), run again (kit busy), Esc
key('ctrl+z'); time.sleep(0.3); key('F5'); time.sleep(7); key('F5'); time.sleep(3); key('Escape'); time.sleep(2)
shot('09_stopped')


open(os.path.join(d, 'quit'), 'w').close()
app.terminate(); sh('wineserver', '-k'); fk.wait(timeout=5)
shutil.rmtree(d, ignore_errors=True)
print('RESULT:', 'ALL PASS' if all(results) else 'FAILURES')
