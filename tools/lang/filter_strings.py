# Narrow analysis/strings_en.tsv to text the player sees, split into lines (the runtime translates line by line).
#  - game strings only (below the image libraries' and the CRT's string tables)
#  - not strings whose only uses are arguments of printf (console output)
#  - not ids: single lowercase words, snake_case, button names with %d
# Usage: python filter_strings.py uplink.exe analysis/export.json analysis/strings_en.tsv analysis/lines_en.tsv
import sys, re, json, struct, pefile, capstone
from capstone import x86_const as X

exe, export, src, out = sys.argv[1:5]
GAME_STR_END = 0x5b2d00
PRINTF = {0x540fb2}            # _printf (Ghidra FID)
pe = pefile.PE(exe)
img = pe.get_memory_mapped_image()
B = pe.OPTIONAL_HEADER.ImageBase
D = json.load(open(export))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

rows = []
for line in open(src, encoding='utf-8'):
    a, t = line.rstrip('\n').split('\t', 1)
    rows.append((int(a, 16), t))
want = set(a for a, _ in rows)

# uses of each string: does a call to printf follow the push within a few instructions?
heads = sorted(D['code_heads'])
uses = {}
for i, h in enumerate(heads):
    o = h - B
    if img[o] != 0x68:
        continue
    v = struct.unpack_from('<I', img, o + 1)[0]
    if v not in want:
        continue
    kind = 'other'
    for h2 in heads[i + 1:i + 14]:
        ins = next(md.disasm(bytes(img[h2 - B:h2 - B + 16]), h2), None)
        if ins and ins.mnemonic == 'call':
            if ins.operands[0].type == X.X86_OP_IMM and (ins.operands[0].imm & 0xffffffff) in PRINTF:
                kind = 'printf'
            break
    uses.setdefault(v, set()).add(kind)


def unesc(t):
    return t.replace('\\n', '\n').replace('\\t', '\t').replace('\\\\', '\\')


def esc(t):
    return t.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def is_id(s):
    if re.fullmatch(r'[a-z][a-z0-9]*', s):
        return True                                    # single lowercase word: id or password
    if re.fullmatch(r'[a-z0-9_ %ds]+', s) and '_' in s:
        return True
    if re.fullmatch(r'[A-Za-z]+[0-9]+', s):
        return True
    if '::' in s or '->' in s or '==' in s or '!=' in s or '(' in s and ')' in s and ' ' not in s:
        return True
    return False


seen = set()
n = 0
with open(out, 'w', encoding='utf-8', newline='\n') as fp:
    for a, t in rows:
        if a >= GAME_STR_END:
            continue
        if uses.get(a) == {'printf'}:
            continue
        for ln in unesc(t).split('\n'):
            s = ln.strip()
            if len(s) < 2 or not re.search(r'[A-Za-z]{2}', s) or is_id(s):
                continue
            if s in seen:
                continue
            seen.add(s)
            fp.write('%x\t%s\n' % (a, esc(s)))
            n += 1
print('lines to translate: %d' % n)
