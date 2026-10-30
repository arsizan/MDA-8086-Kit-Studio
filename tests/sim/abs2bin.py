import sys
def load_abs(path):
    mem = {}
    for line in open(path, 'r', errors='replace'):
        line = line.strip().lstrip('\x1a')
        if not line.startswith(':'): continue
        b = bytes.fromhex(line[1:])
        n, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        assert sum(b) & 0xFF == 0, 'checksum'
        if typ == 1: break
        for i in range(n): mem[addr + i] = b[4 + i]
    lo, hi = min(mem), max(mem) + 1
    return lo, bytes(mem.get(a, 0xFF) for a in range(lo, hi))
if __name__ == '__main__':
    lo, data = load_abs(sys.argv[1])
    open(sys.argv[2], 'wb').write(data)
    print(hex(lo), len(data))
