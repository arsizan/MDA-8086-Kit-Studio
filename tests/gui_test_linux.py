#!/usr/bin/env python3
"""End-to-end GUI test of the Linux program (GTK 3) under Xvfb (:91, no window manager: the
window is at 0,0) with the fake kit on a pseudo-terminal (KIT_TEST_PORTS).
usage: gui_test_linux.py PROGRAM OUTDIR [K28.ABS K27.ABS]
FAKEKIT_ARGS="--nul=g,late" makes the fake kit send invisible NUL bytes.
The output pane is read with Edit > Select All + Copy (xclip)."""
import os, sys, subprocess, time, json, tempfile, shutil
from PIL import Image

prog, outdir = os.path.abspath(sys.argv[1]), sys.argv[2]
S = '/tmp/claude-0/-home-claude/911d5bc8-7aa7-5213-9528-911b2ede0c6f/scratchpad'
k28 = sys.argv[3] if len(sys.argv) > 3 else f'{S}/mda/build/K28.ABS'
k27 = sys.argv[4] if len(sys.argv) > 4 else f'{S}/mda/k27/K27.ABS'
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(outdir, exist_ok=True)
home = tempfile.mkdtemp(prefix='kshome_')
env = dict(os.environ, DISPLAY=':91', NO_AT_BRIDGE='1', GSETTINGS_BACKEND='memory', HOME=home,
           XDG_CACHE_HOME=os.path.join(home, '.cache'))   # (no D-Bus / dconf in the test machine)
MENU = {'file': 19, 'edit': 57, 'kit': 92, 'examples': 146, 'templates': 225, 'help': 288}
ITEM_Y = [37, 62, 87, 112, 137]

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
def menu(name, item, x_item=None):
    click(MENU[name], 12); click(x_item or MENU[name] + 90, ITEM_Y[item]); time.sleep(0.5)
def output_text():
    click(400, 520); key('ctrl+a', 'ctrl+c'); time.sleep(0.3)
    t = sh('xclip', '-o', '-selection', 'clipboard').stdout
    click(400, 250)                       # back to the editor
    return t
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
def editor_dark_pixels(img):
    ed = img.crop((20, 85, 920, 420)).convert('L')
    data = ed.get_flattened_data() if hasattr(ed, 'get_flattened_data') else ed.getdata()
    return sum(1 for v in data if v < 128)
results = []
def check(name, cond):
    results.append(bool(cond)); print(('PASS ' if cond else 'FAIL ') + name, flush=True)

d = tempfile.mkdtemp(prefix='fkgtk_')
fk = subprocess.Popen([sys.executable, os.path.join(here, 'fakekit.py'), d] + os.environ.get('FAKEKIT_ARGS', '').split())
while not os.path.exists(os.path.join(d, 'port')): time.sleep(0.05)
env['KIT_TEST_PORTS'] = open(os.path.join(d, 'port')).read()
app = subprocess.Popen([prog], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(4)
check('window opened', bool(sh('xdotool', 'search', '--name', 'Kit Studio').stdout.split()))
check('editor is empty at start', editor_dark_pixels(shot('01_start')) < 60)

# templates: each one is a new program that assembles without errors
menu('templates', 0, 380)
check('Template 1 fills the editor', editor_dark_pixels(shot('02_template1')) > 500)
key('F7'); time.sleep(2)
t = output_text()
check('Template 1 assembles: OK, 15 bytes', 'OK: 15 bytes of code at 0000:1000.' in t and 'New program from Template 1' in t)
menu('templates', 1, 380); key('F7'); time.sleep(2)
t = output_text()
check('Template 2 assembles: OK, 20 bytes', 'OK: 20 bytes of code at 0000:1000.' in t)

# three-colour example: the kit check draws the rows, no "almost all lit" warning
menu('examples', 1, 240); key('F7'); time.sleep(3); shot('03_three_colour')
t = output_text()
pic = ['RRRRRRRR', '########', '@@@@@@@@'] * 2 + ['RRRRRRRR', '########']
check('three-colour rows: kit check picture', all(row in t for row in pic) and 'Almost all' not in t)

# 7-segment example: F7, File > New is empty, then F5 sends it to the kit
menu('examples', 0, 240); key('F7'); time.sleep(2.5); shot('04_seg7_assembled')
key('ctrl+n'); time.sleep(1)
check('File > New gives an empty editor', editor_dark_pixels(shot('05_new')) < 60)
menu('examples', 0, 240)
key('F5'); time.sleep(6); shot('06_run_seg7')
check('7-segment program arrived in the kit exactly', kit_memory(d) == abs_bytes(k28))

# A-scroll example while the 7-segment program runs -> waits for RESET
menu('examples', 3, 240)
key('F5'); time.sleep(4); shot('07_waiting_for_reset')
open(os.path.join(d, 'reset'), 'w').close(); time.sleep(6); shot('08_after_reset')
check('A-scroll program arrived after RESET', kit_memory(d) == abs_bytes(k27))

# kit command while the program runs -> wait -> RESET -> memory dump
click(400, 630); typetext('d 0000:1000 20'); key('Return'); time.sleep(3)
open(os.path.join(d, 'reset'), 'w').close(); time.sleep(4)
t = output_text()
check('kit command typed in capitals and answered', 'D 0000:1000 20' in t and '0000:1000 B0 80 E6 1E' in t)

# assembler error with a hint (SHL AL,4 after line 6), double-click jumps, Undo removes it
click(400, 250); key('ctrl+Home'); [key('Down', delay=0.1) for _ in range(5)]; key('End', 'Return')
typetext('     SHL AL, 4'); key('F7'); time.sleep(2); shot('09_error_hint')
t = output_text()
check('error line and hint for SHL AL,4', 'DOT_A_SCROLL.ASM(7) : Error' in t and 'Hint (line 7): The 8086 shifts only by 1 or by CL' in t)
key('ctrl+z'); time.sleep(0.5); key('F7'); time.sleep(2)
t = output_text()
check('Undo removes the typed line (assembles again: 76 bytes)', t.rstrip().count('OK: 76 bytes') >= 1 and
      t.rfind('OK: 76 bytes') > t.rfind('Error'))

# File > Save As writes NAME.ASM with CR LF; F7 then writes NAME.ABS next to it
save_dir = home          # the dialog opens in the home folder: type only the name (no '/':
                         # GTK's path completion can eat fast typed characters)
menu('file', 3, 80); time.sleep(1.5); shot('10_save_as_dialog')
key('ctrl+a'); typetext('MYPROG.ASM'); key('Return'); time.sleep(1.5)
click(400, 250); key('F7'); time.sleep(2)     # (no window manager: give the window the focus back)
asm_path, abs_path = os.path.join(save_dir, 'MYPROG.ASM'), os.path.join(save_dir, 'MYPROG.ABS')
data = open(asm_path, 'rb').read() if os.path.exists(asm_path) else b''
check('Save As wrote MYPROG.ASM with CR LF line ends', data.count(b'\r\n') > 30 and data.count(b'\n') == data.count(b'\r\n'))
ok = os.path.exists(abs_path) and abs_bytes(abs_path) == abs_bytes(k27)
check('Assemble wrote MYPROG.ABS (same bytes as the A-scroll program)', ok)
if not ok: print('    files:', os.listdir(save_dir), '\n    output end:', output_text()[-700:])

# Stop while waiting: run (kit at prompt), run again (kit busy -> waits), Esc
key('F5'); time.sleep(6); key('F5'); time.sleep(3); key('Escape'); time.sleep(2); shot('11_stopped')
t = output_text()
check('Esc stops the wait for RESET', 'Stopping ...' in t[-600:] and 'Stopped.' in t[-400:])

# Help > Kit log
menu('help', 2, 330); time.sleep(1)
t = output_text()
check('Help > Kit log names the log file', 'Kit log: ' + os.path.join(home, '.cache', 'kitstudio', 'kit_log.txt') in t)

# change the program, then Ctrl+Q: "save changes?" -> No -> the program ends
click(400, 250); typetext('; x'); key('ctrl+q'); time.sleep(1.5); shot('12_save_question')
key('alt+n'); time.sleep(2)
try:
    app.wait(timeout=5); ended = True
except subprocess.TimeoutExpired:
    ended = False; app.kill()
check('Ctrl+Q asks to save, No ends the program', ended)
out = app.stdout.read() if ended else ''
check('no GTK warnings on the console', 'Gtk-' not in out and 'GLib-' not in out and 'CRITICAL' not in out)
if out.strip(): print('console output:\n' + out)

log_path = os.path.join(home, '.cache', 'kitstudio', 'kit_log.txt')
log = open(log_path, errors='replace').read() if os.path.exists(log_path) else ''
if log: shutil.copy(log_path, os.path.join(outdir, 'kit_log.txt'))
runs = log.count('-- Program is running on the kit')
check(f'kit log: 3 runs confirmed by "Execute Address" (got {runs})', runs == 3)
check('no "did not answer" warning', 'did not answer' not in log)

open(os.path.join(d, 'quit'), 'w').close(); fk.wait(timeout=5)
shutil.rmtree(d, ignore_errors=True); shutil.rmtree(home, ignore_errors=True)
print('RESULT:', 'ALL PASS' if all(results) else 'FAILURES')
