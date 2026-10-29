import sys, os
def parse(path):
    mem = {}
    for line in open(path, 'rb').read().decode('ascii', 'replace').split('\n'):
        line = line.strip().strip('\x1a')
        if not line.startswith(':'): continue
        b = bytes.fromhex(line[1:])
        assert sum(b) & 0xFF == 0, (path, line)
        if b[3] == 0:
            a = (b[1] << 8) | b[2]
            for i in range(b[0]): mem[a + i] = b[4 + i]
    return mem
S = sys.argv[1]; outdir = sys.argv[2]
O = '/mnt/user-data/uploads/mda/8086/Asm8086'
refs = {n: f'{O}/{n}.ABS' for n in ('SAMPLE','FND','MATRIX','MATRIX_3','LED','ADDRE')}
refs.update({'K27': f'{S}/mda/k27/K27.ABS', 'K27_REV': f'{S}/mda/k27/K27_REV.ABS', 'K27_A': f'{S}/mda/k27/K27_A.ABS',
             'SIZAN': f'{S}/mda/sizan/SIZAN.ABS', 'SIZAN_LR': f'{S}/mda/sizan/SIZAN_LR.ABS',
             'K28': f'{S}/mda/build/K28.ABS', 'JOY': f'{S}/mda/joy/JOY.ABS'})
bad = 0
for n, r in sorted(refs.items()):
    p = f'{outdir}/{n}.ABS'
    if not os.path.exists(p): print(f'{n:9s} MISSING'); bad += 1; continue
    same_bytes = parse(p) == parse(r)
    same_text = open(p,'rb').read() == open(r,'rb').read()
    print(f'{n:9s} bytes {"same" if same_bytes else "DIFFERENT"}  text {"identical" if same_text else "differs (record size/^Z)"}')
    bad += not same_bytes
print('ALL BYTES MATCH' if not bad else f'{bad} PROBLEMS')
