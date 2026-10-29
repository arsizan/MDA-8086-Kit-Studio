#!/usr/bin/env python3
"""Run kittest against fakekit in several scenarios. usage: run_proto_tests.py KITTEST_CMD ABS_FILE [wine]"""
import os, sys, subprocess, time, json, shutil, tempfile
kittest, absfile = sys.argv[1], sys.argv[2]
wine = len(sys.argv) > 3 and sys.argv[3] == 'wine'
here = os.path.dirname(os.path.abspath(__file__))

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

def scenario(name, kit_args, test_args, reset_at=None, expect_rc=0, check_mem=True):
    d = tempfile.mkdtemp(prefix='fk_')
    fk = subprocess.Popen([sys.executable, os.path.join(here, 'fakekit.py'), d] + kit_args)
    for _ in range(100):
        if os.path.exists(os.path.join(d, 'port')) and ('--dead-port' not in kit_args or os.path.exists(os.path.join(d, 'deadport'))): break
        time.sleep(0.05)
    port = open(os.path.join(d, 'port')).read()
    ports = [port]
    if '--dead-port' in kit_args:
        ports = [open(os.path.join(d, 'deadport')).read(), port]   # dead port first
    env = dict(os.environ)
    if wine:
        # map COM1, COM2 to the ptys (Wine reads HKLM\Software\Wine\Ports at start)
        for i in range(1, 4):
            subprocess.run(['wine', 'reg', 'delete', r'HKLM\Software\Wine\Ports', '/v', f'COM{i}', '/f'], capture_output=True)
        for i, p in enumerate(ports):
            subprocess.run(['wine', 'reg', 'add', r'HKLM\Software\Wine\Ports', '/v', f'COM{i+1}', '/d', p, '/f'], capture_output=True)
        subprocess.run(['wineserver', '-k'], capture_output=True)
        time.sleep(1)
    else:
        env['KIT_TEST_PORTS'] = ','.join(ports)
    t0 = time.time()
    cmd = (['wine'] if wine else []) + [kittest, absfile] + test_args
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True, errors='replace')
    if reset_at is not None:
        time.sleep(reset_at)
        open(os.path.join(d, 'reset'), 'w').close()
    out, _ = proc.communicate(timeout=120)
    dt = time.time() - t0
    open(os.path.join(d, 'quit'), 'w').close()
    fk.wait(timeout=5)
    ok = proc.returncode == expect_rc
    memok = None
    if check_mem and expect_rc == 0:
        mj = os.path.join(d, 'memory.json')
        got = {int(a, 16): v for a, v in json.load(open(mj)).items()} if os.path.exists(mj) else {}
        memok = got == abs_bytes(absfile)
        ok = ok and memok
    print(f'--- {name}: {"PASS" if ok else "FAIL"} (rc={proc.returncode}, {dt:.1f}s, memory {"matches" if memok else memok})')
    if not ok or os.environ.get('VERBOSE'):
        print(out)
    else:
        print('    ' + '\n    '.join(l for l in out.splitlines() if l.startswith(('[OK]', '[ERR]', '[WARN]', '=='))))
    shutil.rmtree(d, ignore_errors=True)
    return ok

res = []
res.append(scenario('kit ready', [], []))
res.append(scenario('dead port first, kit second', ['--dead-port'], []))
res.append(scenario('kit silent, user presses RESET after 3 s', ['--silent-until-reset'], [], reset_at=3))
res.append(scenario('kit silent, no wait allowed', ['--silent-until-reset'], ['nowait'], expect_rc=1, check_mem=False))
res.append(scenario('user presses Stop while waiting', ['--silent-until-reset'], ['wait', '2500'], expect_rc=1, check_mem=False))
print('ALL PASS' if all(res) else 'SOME FAILED')
