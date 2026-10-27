# Completes the Ghidra export (tools/ghidra/GhidraExport.java) for recomp.py: imports from the PE's import table
# (IAT address -> [dll, name or #ordinal]) and segment names as recomp.py expects them.
# Usage: python fix_export.py uplink.exe analysis/ghidra_export.json analysis/export.json
import json, sys
import pefile

exe, src, dst = sys.argv[1:4]
pe = pefile.PE(exe)
D = json.load(open(src))
imps = {}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in d.imports:
        imps[imp.address] = [d.dll.decode(), imp.name.decode() if imp.name else '#%d' % imp.ordinal]
D['imports'] = imps
segs = []
for s in D['segments']:
    if s['name'] in ('Headers', 'tdb') or s['name'].startswith('EXTERNAL'): continue
    segs.append(s)
D['segments'] = sorted(segs, key=lambda s: s['start'])
# Ghidra auto-names ("FUN_", "LAB_", "DAT_", "switchD_"...) are noise for recomp.py
D['names'] = {k: v for k, v in D['names'].items()
              if not v.startswith(('LAB_', 'DAT_', 'switchD', 'caseD', 'PTR_', 's_', 'u_', 'BYTE_', 'WORD_', 'DWORD_'))}
# Jump tables Ghidra did not resolve (MSVC memcpy/memmove's, a few switches): jmp [reg*4 + table] with the case
# count from the "cmp reg, N; ja" in front of it, or the run of table entries that point at code.
import capstone, struct
from capstone import x86_const as X
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
IMG = pe.get_memory_mapped_image(); BASE = pe.OPTIONAL_HEADER.ImageBase
text = [s for s in D['segments'] if s['name'] == '.text'][0]
heads = sorted(D['code_heads']); hs = set(heads)
def dec(a): return next(md.disasm(bytes(IMG[a - BASE:a - BASE + 16]), a), None)
# Ghidra treats "jmp [iat]" thunks of ordinal imports (no external symbol) as computed jumps: not switches
dropped = 0
for k in list(D['switches']):
    h = int(k)
    if IMG[h - BASE] == 0xFF and IMG[h - BASE + 1] == 0x25 and struct.unpack_from('<I', IMG, h - BASE + 2)[0] in imps:
        del D['switches'][k]; dropped += 1
print('import thunk pseudo-switches dropped: %d' % dropped)
added = 0
for i, h in enumerate(heads):
    if str(h) in D['switches']: continue
    b = IMG[h - BASE:h - BASE + 2]
    if b[0] != 0xFF or (b[1] & 0x38) != 0x20: continue
    ins = dec(h)
    if not ins or ins.mnemonic != 'jmp' or ins.operands[0].type != X.X86_OP_MEM: continue
    m = ins.operands[0].mem
    if not m.index or m.scale != 4 or m.base: continue
    tab = m.disp & 0xffffffff
    n = None
    for back in heads[max(0, i - 4):i][::-1]:
        p = dec(back)
        if p and p.mnemonic == 'cmp' and p.operands[1].type == X.X86_OP_IMM and p.operands[0].type == X.X86_OP_REG \
                and p.operands[0].reg == m.index:
            n = p.operands[1].imm + 1; break
    tg = []
    for k in range(n if n else 1024):
        o = tab + 4 * k - BASE
        if o < 0 or o + 4 > len(IMG): break
        v = struct.unpack_from('<I', IMG, o)[0]
        if not (text['start'] <= v < text['end']):
            if n: print('warning: table %x entry %d -> %x' % (tab, k, v))
            break
        tg.append(v)
    if tg:
        D['switches'][str(h)] = {'targets': sorted(set(tg)), 'jumps': tab, 'ncases': len(tg), 'elbase': 0}
        for t in tg:
            if t not in hs: D['code_heads'].append(t); hs.add(t)
        added += 1
print('jump tables added: %d' % added)
json.dump(D, open(dst, 'w'))
print('funcs %d heads %d switches %d imports %d segments %s' % (
    len(D['funcs']), len(D['code_heads']), len(D['switches']), len(imps), [s['name'] for s in D['segments']]))
