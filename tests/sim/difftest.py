"""Differential test: kitsim.c CPU core vs Unicorn (QEMU) on single random 8086 instructions.
Flags that the 8086 leaves undefined for an instruction are masked out."""
import ctypes, random, sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UcError
from unicorn.x86_const import *

lib = ctypes.CDLL('./libkitsim.so')
lib.kitsim_test.restype = ctypes.c_int
lib.kitsim_test.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ushort), ctypes.c_uint, ctypes.c_uint,
                            ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulonglong)]
MEM = 0x100000
CF, PF, AF, ZF, SF, TF, IF, DF, OF = 1, 4, 0x10, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800
STATUS = CF | PF | AF | ZF | SF | OF
REGS = [UC_X86_REG_AX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_BX, UC_X86_REG_SP, UC_X86_REG_BP,
        UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_SS, UC_X86_REG_DS]
NAMES = 'AX CX DX BX SP BP SI DI ES CS SS DS IP FL'.split()
rnd = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
base_mem = bytearray(rnd.getrandbits(8) for _ in range(MEM))
for a in range(0xF0000, MEM): base_mem[a] = 0xFF
mu = Uc(UC_ARCH_X86, UC_MODE_16)
mu.mem_map(0, MEM)
from unicorn import UC_HOOK_INTR
INTR = []
def _intr(uc, n, user):
    INTR.append(n); uc.emu_stop()
mu.hook_add(UC_HOOK_INTR, _intr)

def modrm_bytes(allow_reg=True, mem_only=False):
    mod = rnd.choice([0, 1, 2] + ([] if mem_only else [3, 3]))
    reg = rnd.randrange(8); rm = rnd.randrange(8)
    b = [(mod << 6) | (reg << 3) | rm]
    if mod == 0 and rm == 6: b += [rnd.getrandbits(8), rnd.getrandbits(8)]
    elif mod == 1: b += [rnd.getrandbits(8)]
    elif mod == 2: b += [rnd.getrandbits(8), rnd.getrandbits(8)]
    return b, mod, reg, rm

def gen():
    """returns (bytes, undefined-flag mask, description)"""
    prefix = []
    if rnd.random() < 0.15: prefix = [rnd.choice([0x26, 0x2E, 0x36, 0x3E])]
    kind = rnd.randrange(22)
    imm8 = lambda: [rnd.getrandbits(8)]
    imm16 = lambda: [rnd.getrandbits(8), rnd.getrandbits(8)]
    if kind == 0:                       # ALU r/m,reg / reg,r/m
        op = rnd.choice([o for o in range(0x40) if (o & 7) < 4])
        m, mod, reg, rm = modrm_bytes()
        und = AF if (op >> 3) in (1, 4, 6) else 0
        return prefix + [op] + m, und, 'alu %02X' % op
    if kind == 1:                       # ALU acc,imm
        op = rnd.choice([o for o in range(0x40) if (o & 7) in (4, 5)])
        und = AF if (op >> 3) in (1, 4, 6) else 0
        return [op] + (imm8() if op & 1 == 0 else imm16()), und, 'alui %02X' % op
    if kind == 2:                       # group 1
        op = rnd.choice([0x80, 0x81, 0x82, 0x83])
        m, mod, reg, rm = modrm_bytes()
        und = AF if reg in (1, 4, 6) else 0
        return prefix + [op] + m + (imm16() if op == 0x81 else imm8()), und, 'grp1 %02X/%d' % (op, reg)
    if kind == 3:                       # INC/DEC/PUSH/POP reg (no PUSH SP)
        op = rnd.choice(list(range(0x40, 0x54)) + list(range(0x55, 0x60)))
        return [op], 0, 'r16 %02X' % op
    if kind == 4:                       # TEST/XCHG/MOV r/m
        op = rnd.choice([0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B])
        m, mod, reg, rm = modrm_bytes()
        return prefix + [op] + m, AF if op < 0x86 else 0, 'mov %02X' % op
    if kind == 5:                       # MOV sreg (no CS load)
        op = rnd.choice([0x8C, 0x8E])
        m, mod, reg, rm = modrm_bytes()
        reg = rnd.choice([0, 2, 3]) if op == 0x8E else rnd.randrange(4)
        m[0] = (m[0] & 0xC7) | (reg << 3)
        return prefix + [op] + m, 0, 'sreg %02X' % op
    if kind == 6:                       # LEA / LES / LDS
        op = rnd.choice([0x8D, 0xC4, 0xC5])
        m, mod, reg, rm = modrm_bytes(mem_only=True)
        return prefix + [op] + m, 0, 'lea %02X' % op
    if kind == 7:                       # misc one-byte
        op = rnd.choice([0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9C, 0x9E, 0x9F,
                         0xF5, 0xF8, 0xF9, 0xFC, 0xFD, 0x27, 0x2F, 0xD7])
        und = OF if op in (0x27, 0x2F) else 0
        return prefix + [op], und, 'misc %02X' % op
    if kind == 8:                       # MOV moffs
        op = rnd.choice([0xA0, 0xA1, 0xA2, 0xA3])
        return prefix + [op] + imm16(), 0, 'moffs %02X' % op
    if kind == 9:                       # MOV reg,imm
        op = rnd.randrange(0xB0, 0xC0)
        return [op] + (imm8() if op < 0xB8 else imm16()), 0, 'movi %02X' % op
    if kind == 10:                      # MOV r/m,imm
        op = rnd.choice([0xC6, 0xC7])
        m, mod, reg, rm = modrm_bytes()
        m[0] &= 0xC7
        return prefix + [op] + m + (imm8() if op == 0xC6 else imm16()), 0, 'movrmi %02X' % op
    if kind == 11:                      # shifts by 1
        op = rnd.choice([0xD0, 0xD1])
        m, mod, reg, rm = modrm_bytes()
        reg = rnd.choice([0, 1, 2, 3, 4, 5, 7]); m[0] = (m[0] & 0xC7) | (reg << 3)
        return prefix + [op] + m, AF, 'shift1 %02X/%d' % (op, reg)
    if kind == 12:                      # shifts by CL (CL set small below)
        op = rnd.choice([0xD2, 0xD3])
        m, mod, reg, rm = modrm_bytes()
        reg = rnd.choice([0, 1, 2, 3, 4, 5, 7]); m[0] = (m[0] & 0xC7) | (reg << 3)
        return prefix + [op] + m, AF | OF, 'shiftcl %02X/%d' % (op, reg)
    if kind == 13:                      # group 3: TEST NOT NEG
        op = rnd.choice([0xF6, 0xF7])
        m, mod, reg, rm = modrm_bytes()
        reg = rnd.choice([0, 2, 3]); m[0] = (m[0] & 0xC7) | (reg << 3)
        extra = (imm8() if op == 0xF6 else imm16()) if reg == 0 else []
        return prefix + [op] + m + extra, AF if reg == 0 else 0, 'grp3 %02X/%d' % (op, reg)
    if kind == 14:                      # MUL IMUL
        op = rnd.choice([0xF6, 0xF7])
        m, mod, reg, rm = modrm_bytes()
        reg = rnd.choice([4, 5]); m[0] = (m[0] & 0xC7) | (reg << 3)
        return prefix + [op] + m, SF | ZF | AF | PF, 'mul %02X/%d' % (op, reg)
    if kind == 15:                      # DIV IDIV
        op = rnd.choice([0xF6, 0xF7])
        m, mod, reg, rm = modrm_bytes()
        reg = rnd.choice([6, 7]); m[0] = (m[0] & 0xC7) | (reg << 3)
        return prefix + [op] + m, STATUS, 'div %02X/%d' % (op, reg)
    if kind == 16:                      # INC/DEC r/m, PUSH r/m
        op = rnd.choice([0xFE, 0xFF])
        m, mod, reg, rm = modrm_bytes()
        reg = rnd.choice([0, 1] + ([6] if op == 0xFF else [])); m[0] = (m[0] & 0xC7) | (reg << 3)
        if op == 0xFF and reg == 6 and (m[0] >> 6) == 3 and (m[0] & 7) == 4: m[0] = (m[0] & 0xF8) | 3
        return prefix + [op] + m, 0, 'grp45 %02X/%d' % (op, reg)
    if kind == 17:                      # string ops, optionally REP (CX kept small below)
        op = rnd.choice([0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF])
        rep = rnd.choice([[], [0xF3], [0xF2]])
        return rep + prefix + [op], 0, 'str %s%02X' % ('rep ' if rep else '', op)
    if kind == 18:                      # AAM AAD
        op = rnd.choice([0xD4, 0xD5])
        return [op, rnd.choice([10, rnd.randrange(1, 256)])], OF | AF | CF, 'aam %02X' % op
    if kind == 19:                      # POP r/m, PUSH sreg, POP sreg(no CS)
        c = rnd.randrange(3)
        if c == 0:
            m, mod, reg, rm = modrm_bytes(); m[0] &= 0xC7
            if (m[0] >> 6) == 3 and (m[0] & 7) == 4: m[0] = (m[0] & 0xF8) | 3
            return prefix + [0x8F] + m, 0, 'popm'
        if c == 1: return [rnd.choice([0x06, 0x0E, 0x16, 0x1E])], 0, 'pushs'
        return [rnd.choice([0x07, 0x1F])], 0, 'pops'
    if kind == 20:                      # CBW CWD SAHF LAHF + flags ops
        return [rnd.choice([0x98, 0x99, 0x9E, 0x9F, 0xF8, 0xF9, 0xF5])], 0, 'flg'
    # kind 21: conditional jumps / LOOP family / JMP / CALL near (target stays near; only IP compared)
    op = rnd.choice(list(range(0x70, 0x80)) + [0xE0, 0xE1, 0xE2, 0xE3, 0xEB])
    return [op, rnd.getrandbits(8)], 0, 'jcc %02X' % op

def run_unicorn(code_addr, regs, nbytes, rep=False):
    mu.mem_write(0, bytes(base_mem))
    mu.ctl_remove_cache(code_addr, code_addr + 16)
    for r, v in zip(REGS, regs[:12]): mu.reg_write(r, v)
    mu.reg_write(UC_X86_REG_IP, regs[12])
    mu.reg_write(UC_X86_REG_EFLAGS, regs[13])
    INTR.clear()
    try:
        if rep:   # QEMU counts every REP iteration; run until the next instruction
            mu.emu_start(regs[9] * 16 + regs[12], code_addr + nbytes, count=100)
        else:
            mu.emu_start(regs[9] * 16 + regs[12], 0xFFFFFFFF, count=1)
    except UcError as e:
        return None, str(e)
    if INTR:
        return None, 'interrupt %d' % INTR[0]
    out = [mu.reg_read(r) & 0xFFFF for r in REGS] + [mu.reg_read(UC_X86_REG_IP) & 0xFFFF, mu.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF]
    return out, bytes(mu.mem_read(0, MEM))

ntests = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
fails = skipped = 0
for t in range(ntests):
    code, und, desc = gen()
    regs = [rnd.getrandbits(16) for _ in range(8)]
    segs = [rnd.randrange(0x0000, 0xD000) for _ in range(4)]
    segs[1] = 0x0100                       # CS
    ip = 0x0000
    regs[4] = rnd.randrange(0x0100, 0xFFF0) & 0xFFFE  # SP away from the wrap
    if 'shiftcl' in desc: regs[1] = (regs[1] & 0xFF00) | rnd.randrange(0, 8 if code[-1] in (0xD2,) else 16)
    if 'shiftcl' in desc: regs[1] = (regs[1] & 0xFF00) | rnd.randrange(0, 8 if (0xD2 in code) else 16)
    if 'str' in desc: regs[1] = rnd.randrange(0, 6)
    fl = 0x0002 | (rnd.getrandbits(12) & (CF | PF | AF | ZF | SF | DF | OF))
    st = regs + segs + [ip, fl]
    code_addr = segs[1] * 16 + ip
    mem = bytearray(base_mem)
    mem[code_addr:code_addr + len(code)] = bytes(code)
    base_mem[code_addr:code_addr + len(code)] = bytes(code)
    # Unicorn
    u, umem = run_unicorn(code_addr, st, len(code), rep=('str rep' in desc))
    # kitsim
    arr = (ctypes.c_ushort * 14)(*st)
    buf = (ctypes.c_ubyte * MEM).from_buffer(mem)
    cyc = ctypes.c_ulonglong(0)
    stop = lib.kitsim_test(ctypes.addressof(buf), arr, code_addr, code_addr + len(code), 1, ctypes.byref(cyc))
    del buf
    k = list(arr)
    # restore base memory for the next test
    base_mem[code_addr:code_addr + len(code)] = mem[code_addr:code_addr + len(code)] if False else base_mem[code_addr:code_addr + len(code)]
    if u is None or stop >= 0:
        skipped += 1      # divide error etc.
        if (u is None) != (stop >= 0):
            print('ASYM', desc, ' '.join('%02X' % b for b in code), 'unicorn:', 'fault' if u is None else 'ok', 'kitsim stop:', stop,
                  '| start', ' '.join('%s=%04X' % (n, v) for n, v in zip(NAMES, st)), '| unicorn end', ' '.join('%s=%04X' % (n, v) for n, v in zip(NAMES, u)) if u else '')
        continue
    mask_fl = 0x0FD5 & ~und & ~(TF | IF)
    diffs = []
    for i in range(13):
        if u[i] != k[i]: diffs.append('%s u=%04X k=%04X' % (NAMES[i], u[i], k[i]))
    if (u[13] & mask_fl) != (k[13] & mask_fl):
        diffs.append('FL u=%04X k=%04X (mask %04X)' % (u[13], k[13], mask_fl))
    if 'misc 9C' in desc and umem != bytes(mem):   # 8086 PUSHF sets flag bits 12-15
        sp = (k[4]) & 0xFFFF; a = (k[10] * 16 + sp + 1) & 0xFFFFF
        umem = bytearray(umem); umem[a] |= 0xF0; umem = bytes(umem)
    if umem != bytes(mem):
        idx = next(i for i in range(MEM) if umem[i] != mem[i])
        diffs.append('MEM @%05X u=%02X k=%02X' % (idx, umem[idx], mem[idx]))
    if diffs:
        fails += 1
        if fails <= 25:
            print('FAIL', desc, ' '.join('%02X' % b for b in code), '| start', ' '.join('%s=%04X' % (n, v) for n, v in zip(NAMES, st)))
            print('     ', '; '.join(diffs))
print('tests=%d fails=%d skipped=%d' % (ntests, fails, skipped))
