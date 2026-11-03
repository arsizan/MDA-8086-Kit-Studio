#!/usr/bin/env python3
"""Make src/kitbuiltin.c (the examples and templates built into the program) from the
.ASM files in src/builtin, and src/icon_png.h (window icon for Linux) from res/icon_preview.png.
Run it after changing a file in src/builtin:   python3 tools/gen_builtin.py"""
import os, io

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')

# (list, source file, menu text, file name shown for the copy; None = new untitled program)
ITEMS = [
    ('examples', 'EX1_SEG7_COUNT.ASM', '1. Seven-segment: count 0-9', 'SEG7_COUNT.ASM'),
    ('examples', 'EX2_DOT_3COLOUR.ASM', '2. Dot matrix: three-colour rows (all 8x8)', 'DOT_3COLOUR.ASM'),
    ('examples', 'EX3_DOT_A.ASM', '3. Dot matrix: letter A', 'DOT_A.ASM'),
    ('examples', 'EX4_DOT_A_SCROLL.ASM', '4. Dot matrix: letter A scrolling', 'DOT_A_SCROLL.ASM'),
    ('templates', 'TEMPLATE1.ASM', 'Template 1 - Short form (.CODE only, tables with CS:)', None),
    ('templates', 'TEMPLATE2.ASM', 'Template 2 - Book style (.DATA, MOV DS,AX)', None),
]


def c_string(text):
    lines = text.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    out = []
    for l in lines:
        assert all(32 <= ord(ch) < 127 or ch == '\t' for ch in l), 'only ASCII in built-in programs'
        out.append('    "' + l.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t') + '\\n"')
    return '\n'.join(out)


def main():
    parts = ['/* kitbuiltin.c - made by tools/gen_builtin.py from the .ASM files in src/builtin - do not edit by hand */',
             '#include <stddef.h>', '#include "kitbuiltin.h"', '']
    names = {}
    for i, (lst, fname, menu, shown) in enumerate(ITEMS):
        text = open(os.path.join(SRC, 'builtin', fname), encoding='ascii').read()
        var = 'text%d' % i
        names[i] = var
        parts.append('/* %s */' % fname)
        parts.append('static const char %s[] =\n%s;\n' % (var, c_string(text)))
    for lst in ('examples', 'templates'):
        rows = [(i, it) for i, it in enumerate(ITEMS) if it[0] == lst]
        parts.append('const KitBuiltin kit_%s[] = {' % lst)
        for i, (_, fname, menu, shown) in rows:
            parts.append('    { "%s", %s, %s },' % (menu, '"%s"' % shown if shown else 'NULL', names[i]))
        parts.append('};')
        parts.append('const int kit_n%s = %d;\n' % (lst, len(rows)))
    open(os.path.join(SRC, 'kitbuiltin.c'), 'w', newline='\n').write('\n'.join(parts))

    # window icon for the Linux program: 64x64 PNG as bytes
    from PIL import Image
    im = Image.open(os.path.join(HERE, '..', 'res', 'icon_preview.png')).convert('RGBA')
    im = im.resize((64, 64), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, 'PNG', optimize=True)
    data = buf.getvalue()
    rows = [', '.join('0x%02X' % b for b in data[k:k + 16]) for k in range(0, len(data), 16)]
    open(os.path.join(SRC, 'icon_png.h'), 'w', newline='\n').write(
        '/* icon_png.h - made by tools/gen_builtin.py from res/icon_preview.png (64x64 PNG) */\n'
        'static const unsigned char icon_png[%d] = {\n    %s\n};\n' % (len(data), ',\n    '.join(rows)))
    print('wrote src/kitbuiltin.c (%d items) and src/icon_png.h (%d bytes)' % (len(ITEMS), len(data)))


if __name__ == '__main__':
    main()
