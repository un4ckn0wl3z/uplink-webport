// Ghidra headless exporter producing the same JSON shape as the skill's ida_export.py (funcs, code_heads,
// switches, names, segments; imports are filled in by tools/fix_export.py from the PE itself).
//   analyzeHeadless <proj_dir> uplink -import uplink.exe -scriptPath tools/ghidra
//       -postScript GhidraExport.java analysis/ghidra_export.json
// Before exporting, functions reached only through data (vtables, callback tables) or immediates are created
// when the address follows other code (ret/jmp/int3/nop padding), like ida_export.py does.
// @category Port
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.scalar.Scalar;
import java.io.PrintWriter;
import java.util.*;

public class GhidraExport extends GhidraScript {
    long tlo, thi;
    Listing L;

    boolean inText(long v) { return v >= tlo && v < thi; }

    int byteAt(long a) {
        try { return getByte(toAddr(a)) & 0xff; } catch (Exception e) { return -1; }
    }

    boolean plausibleStart(long t) {
        Address a = toAddr(t);
        if (L.getInstructionAt(a) != null || getFunctionContaining(a) != null) return false;
        if (L.getInstructionContaining(a) != null) return false;
        int pb = byteAt(t - 1);
        if (pb == 0xCC || pb == 0x90) return true;
        Instruction p = L.getInstructionContaining(toAddr(t - 1));
        if (p != null && p.getMaxAddress().getOffset() == t - 1) {
            String m = p.getMnemonicString().toLowerCase();
            return m.startsWith("ret") || m.equals("jmp") || m.equals("int3");
        }
        Function f = getFunctionContaining(toAddr(t - 1));
        return f != null && f.getBody().getMaxAddress().getOffset() == t - 1;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        L = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = null;
        for (MemoryBlock b : mem.getBlocks()) if (b.isExecute()) { text = b; break; }
        tlo = text.getStart().getOffset();
        thi = text.getEnd().getOffset() + 1;

        for (int rnd = 0; rnd < 6; rnd++) {
            TreeSet<Long> cands = new TreeSet<>();
            for (MemoryBlock b : mem.getBlocks()) {
                if (b.isExecute() || !b.isInitialized()) continue;
                long s = b.getStart().getOffset(), e = b.getEnd().getOffset() + 1;
                for (long a = s; a + 4 <= e; a += 4) {
                    long v = getInt(toAddr(a)) & 0xffffffffL;
                    if (inText(v)) cands.add(v);
                }
            }
            for (Instruction ins : L.getInstructions(true)) {
                for (int i = 0; i < ins.getNumOperands(); i++) {
                    for (Object o : ins.getOpObjects(i)) {
                        if (o instanceof Scalar) {
                            long v = ((Scalar) o).getUnsignedValue();
                            if (inText(v)) cands.add(v);
                        }
                    }
                }
            }
            int n = 0;
            for (long t : cands) {
                if (!plausibleStart(t)) continue;
                Address a = toAddr(t);
                if (disassemble(a) && createFunction(a, null) != null) n++;
            }
            analyzeChanges(currentProgram);
            println("export: created " + n + " functions referenced from data (round " + rnd + ")");
            if (n == 0) break;
        }

        PrintWriter out = new PrintWriter(args[0]);
        out.print("{\"segments\": [");
        boolean first = true;
        for (MemoryBlock b : mem.getBlocks()) {
            if (!first) out.print(", ");
            first = false;
            int perm = (b.isRead() ? 4 : 0) | (b.isWrite() ? 2 : 0) | (b.isExecute() ? 1 : 0);
            out.print("{\"start\": " + b.getStart().getOffset() + ", \"end\": " + (b.getEnd().getOffset() + 1)
                    + ", \"name\": \"" + b.getName() + "\", \"perm\": " + perm + "}");
        }
        out.print("], \"funcs\": [");
        first = true;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isExternal() || f.isThunk() && !inText(f.getEntryPoint().getOffset())) continue;
            long st = f.getEntryPoint().getOffset();
            if (!inText(st)) continue;
            StringBuilder ch = new StringBuilder();
            long end = st + 1;
            for (AddressRange r : f.getBody().getAddressRanges()) {
                long a = r.getMinAddress().getOffset(), e = r.getMaxAddress().getOffset() + 1;
                if (ch.length() > 0) ch.append(", ");
                ch.append("[" + a + ", " + e + "]");
                if (e > end) end = e;
            }
            if (!first) out.print(", ");
            first = false;
            out.print("{\"start\": " + st + ", \"end\": " + end + ", \"name\": \"" + f.getName().replace("\"", "'")
                    + "\", \"flags\": 0, \"chunks\": [" + ch + "], \"frsize\": 0, \"argsize\": 0}");
        }
        out.print("], \"code_heads\": [");
        first = true;
        StringBuilder sw = new StringBuilder();
        for (Instruction ins : L.getInstructions(toAddr(tlo), true)) {
            long a = ins.getAddress().getOffset();
            if (!inText(a)) break;
            if (!first) out.print(", ");
            first = false;
            out.print(a);
            FlowType ft = ins.getFlowType();
            if (ft.isJump() && ft.isComputed()) {
                Address[] fl = ins.getFlows();
                if (fl != null && fl.length > 0) {
                    TreeSet<Long> tg = new TreeSet<>();
                    for (Address x : fl) tg.add(x.getOffset());
                    long table = 0;
                    for (Reference r : ins.getReferencesFrom())
                        if (r.getReferenceType().isData() || r.getReferenceType().isRead()) table = r.getToAddress().getOffset();
                    if (sw.length() > 0) sw.append(", ");
                    StringBuilder t = new StringBuilder();
                    for (long x : tg) { if (t.length() > 0) t.append(", "); t.append(x); }
                    sw.append("\"" + a + "\": {\"targets\": [" + t + "], \"jumps\": " + table + ", \"ncases\": "
                            + tg.size() + ", \"elbase\": 0}");
                }
            }
        }
        out.print("], \"switches\": {" + sw + "}, \"names\": {");
        first = true;
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(false);
        HashSet<Long> done = new HashSet<>();
        while (it.hasNext()) {
            Symbol s = it.next();
            if (s.isExternal()) continue;
            long a = s.getAddress().getOffset();
            if (!s.getAddress().isMemoryAddress() || done.contains(a)) continue;
            if (!s.isPrimary()) continue;
            done.add(a);
            if (!first) out.print(", ");
            first = false;
            out.print("\"" + a + "\": \"" + s.getName().replace("\\", "\\\\").replace("\"", "'") + "\"");
        }
        out.print("}, \"imports\": {}}");
        out.close();
        println("export written");
    }
}
