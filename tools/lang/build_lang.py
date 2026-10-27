# Build lang/<lang>/strings.tsv from the translation batches in lang/<lang>/src/*.tsv ("english<TAB>translation",
# \n \t \\ escaped, '#' comments). Checks that every printf conversion of the English survives in the translation
# (same conversions in order, or positional %1$s forms), and reports duplicates that disagree.
# Usage: python build_lang.py lang/th
import sys, os, re, glob

root = sys.argv[1]
conv = re.compile(r'%(?:(\d)\$)?[-+ #0-9.l]*([sducixXf%])')


def convs(s):
    out = []
    for m in conv.finditer(s):
        if m.group(2) == '%':
            continue
        out.append((m.group(1), m.group(2)))
    return out


table, where, bad = {}, {}, 0
for path in sorted(glob.glob(os.path.join(root, 'src', '*.tsv'))):
    for no, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\n').rstrip('\r')
        if not line or line.startswith('#'):
            continue
        if '\t' not in line:
            print('%s:%d: no tab' % (path, no)); bad += 1
            continue
        en, tr = line.split('\t', 1)
        if not tr.strip():
            continue
        ce, ct = convs(en), convs(tr)
        if any(p for p, _ in ct):
            ok = sorted(c for _, c in ce) == sorted(c for _, c in ct)
        else:
            ok = [c for _, c in ce] == [c for _, c in ct]
        if not ok:
            print('%s:%d: conversions differ: %r -> %r' % (path, no, en, tr)); bad += 1
            continue
        if en in table and table[en] != tr:
            print('%s:%d: duplicate with a different translation (first in %s): %r' % (path, no, where[en], en))
        table.setdefault(en, tr); where.setdefault(en, '%s:%d' % (os.path.basename(path), no))
with open(os.path.join(root, 'strings.tsv'), 'w', encoding='utf-8', newline='\n') as fp:
    fp.write('# built by tools/lang/build_lang.py from src/*.tsv\n')
    for en, tr in table.items():
        fp.write('%s\t%s\n' % (en, tr))
print('%d entries, %d problems' % (len(table), bad))
