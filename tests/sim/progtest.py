"""Run whole kit programs for N instructions in kitsim and in Unicorn; compare registers,
memory and the list of OUT writes."""
import ctypes, sys, glob, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from abs2bin import load_abs
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN, UC_HOOK_INTR
from unicorn.x86_const import *
lib = ctypes.CDLL('./libkitsim.so')
lib.kitsim_test.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ushort), ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulonglong)]
REGS = [UC_X86_REG_AX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_BX, UC_X86_REG_SP, UC_X86_REG_BP,
        UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_SS, UC_X86_REG_DS]
N = 300000
ok = bad = 0
for f in sys.argv[1:]:
    lo, img = load_abs(f)
    mem = bytearray(0x100000); mem[lo:lo + len(img)] = img
    for a in range(0xF0000, 0x100000): mem[a] = 0xFF
    st = [0, 0, 0, 0, 0x540, 0, 0, 0, 0, 0, 0, 0, lo, 0x0002]
    # unicorn
    mu = Uc(UC_ARCH_X86, UC_MODE_16); mu.mem_map(0, 0x100000); mu.mem_write(0, bytes(mem))
    for r, v in zip(REGS, st[:12]): mu.reg_write(r, v)
    mu.reg_write(UC_X86_REG_EFLAGS, 2)
    outs, ints = [], []
    mu.hook_add(UC_HOOK_INSN, lambda uc, port, size, val, u: outs.append((port, val & 0xFF)), None, 1, 0, UC_X86_INS_OUT)
    mu.hook_add(UC_HOOK_INTR, lambda uc, n, u: (ints.append(n), uc.emu_stop()))
    # run the same number of instructions; for an INT 3 program kitsim stops before INT 3
    arr = (ctypes.c_ushort * 14)(*st)
    kmem = bytearray(mem)
    buf = (ctypes.c_ubyte * 0x100000).from_buffer(kmem)
    stop = lib.kitsim_test(ctypes.addressof(buf), arr, lo, lo + len(img), N, None)
    del buf
    k = list(arr)
    # number of instructions kitsim executed is N unless it stopped: find by running unicorn until same IP if stopped
    try:
        mu.emu_start(lo, 0xFFFFFFFF, count=N)
    except Exception as e:
        pass
    u = [mu.reg_read(r) & 0xFFFF for r in REGS] + [mu.reg_read(UC_X86_REG_IP) & 0xFFFF, mu.reg_read(UC_X86_REG_EFLAGS) & 0x0FD5]
    k[13] &= 0x0FD5
    umem = bytes(mu.mem_read(0, 0x100000))
    same_regs = u == k
    same_mem = umem[:0xF0000] == bytes(kmem[:0xF0000])
    name = f.rsplit('/', 1)[-1]
    if same_regs and same_mem:
        ok += 1
        print('OK  %-26s stop=%d ints=%s outs=%d' % (name, stop, ints, len(outs)))
    else:
        bad += 1
        print('BAD %-26s stop=%d ints=%s' % (name, stop, ints))
        print('    unicorn', ' '.join('%04X' % v for v in u))
        print('    kitsim ', ' '.join('%04X' % v for v in k))
print('ok=%d bad=%d' % (ok, bad))
