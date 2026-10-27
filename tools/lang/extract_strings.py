# Extract the game's user-visible English strings: NUL-terminated strings in .rdata/.data that code or data
# references, filtered for UI text. Writes a TSV: address, text (escaped \n \t \\).
# The output is derived from the user's copy of the game: keep it (and translations built from it) private.
# Usage: python extract_strings.py uplink.exe analysis/export.json analysis/strings_en.tsv
import sys, re, json, struct, pefile, capstone

exe, export, out = sys.argv[1:4]
pe = pefile.PE(exe)
img = pe.get_memory_mapped_image()
B = pe.OPTIONAL_HEADER.ImageBase
D = json.load(open(export))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
data_secs = [(B + s.VirtualAddress, B + s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections
             if s.Name.rstrip(b'\0') in (b'.rdata', b'.data')]


def in_data(a):
    return any(lo <= a < hi for lo, hi in data_secs)


refs = set()
for h in D['code_heads']:
    o = h - B
    b = bytes(img[o:o + 16])
    ins = next(md.disasm(b, h), None)
    if not ins:
        continue
    for k in range(1, ins.size - 3):
        v = struct.unpack_from('<I', b, k)[0]
        if in_data(v):
            refs.add(v)
for lo, hi in data_secs:
    for a in range(lo, hi - 4, 4):
        v = struct.unpack_from('<I', img, a - B)[0]
        if in_data(v):
            refs.add(v)


def cstr(a):
    o = a - B
    e = img.find(b'\0', o, o + 8192)
    if e <= o:
        return None
    t = img[o:e].decode('latin-1')
    if any(ord(c) < 32 and c not in '\n\t\r' for c in t):
        return None
    if any(ord(c) > 126 for c in t):
        return None
    return t


rows = {}
for a in sorted(refs):
    t = cstr(a)
    if t is None or len(t) < 2:
        continue
    if img[a - B - 1] != 0 and (a - 1) not in refs:
        continue          # the tail of another string
    rows[a] = t

PATH_PREFIXES = ('%s/', '%s\\', 'c:\\', '.\\', './', 'data/', 'graphics/', 'sounds/', 'music/', 'fonts/')


def visible(t):
    s = t.strip()
    if len(s) < 2 or not re.search(r'[A-Za-z]{2}', s):
        return False
    if ' ' not in s and re.search(r'\.(cpp|h|tif|bmp|wav|uni|txt|dat|usr|ttf|dll|exe|tmp|ini|log|sav)\b', s, re.I):
        return False
    if re.match(r'^[a-z0-9_]+$', s) and '_' in s:
        return False                                     # option and object ids
    if ' ' not in s and re.match(r'^[A-Za-z0-9_]+$', s) and re.search(r'[a-z][A-Z]', s):
        return False                                     # CamelCase ids
    if 'Assertion' in s or 'snprintf Failure' in s:
        return False
    if s.startswith(PATH_PREFIXES):
        return False
    return True


def esc(t):
    return t.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r')


n = 0
with open(out, 'w', encoding='utf-8', newline='\n') as fp:
    for a, t in rows.items():
        if visible(t):
            fp.write('%x\t%s\n' % (a, esc(t)))
            n += 1
print('referenced strings %d, visible candidates %d' % (len(rows), n))
