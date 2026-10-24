// Post-script for a raw ELFR module imported with the BinaryLoader at 0x408130 (see import_seq.ps1).
// Rebuilds the linked layout and applies the annotations written by `elfr.py --ghidra`:
//   code   file [0, text)        at 0x408130 (as imported)
//   data   file [text, len) + bss/stack up to the VM window end, moved to the TSV's data address (gap 0xd0/0xb0)
//   alias  the data block again 0x40000 higher (byte-mapped): lui 0x45/0x46 reach the same bytes as 0x41/0x42
//   natives 0x400000-0x40812f, labels native_<i>_<name> at 0x400144 + 64*i
// gp is set to the stack base W over the code, so gp-relative loads resolve.
// Usage: -postScript ImportElfr.java <annotations.tsv>
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class ImportElfr extends GhidraScript {
    long hex(String s) { return Long.decode(s); }

    @Override
    public void run() throws Exception {
        List<String[]> rows = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(getScriptArgs()[0]),
                StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) if (!line.isEmpty()) rows.add(line.split("\t", -1));
        }
        String[] mod = rows.get(0);
        long len = hex(mod[2]), text = hex(mod[3]), code = hex(mod[4]), data = hex(mod[5]), end = hex(mod[6]),
             gp = hex(mod[7]);
        Memory mem = currentProgram.getMemory();
        MemoryBlock blk = mem.getBlock(toAddr(code));
        blk.setName("code");
        blk.setExecute(true);
        mem.split(blk, toAddr(code + text));
        MemoryBlock dblk = mem.getBlock(toAddr(code + text));
        mem.moveBlock(dblk, toAddr(data), monitor);
        dblk = mem.getBlock(toAddr(data));
        dblk.setName("data");
        dblk.setWrite(true);
        dblk.setExecute(false);
        long dataEnd = data + (len - text);
        if (end > dataEnd) mem.createUninitializedBlock("bss_stack", toAddr(dataEnd), end - dataEnd, false).setWrite(true);
        mem.createByteMappedBlock("data_alias_45", toAddr(data + 0x40000), toAddr(data), end - data, false);
        // natives: one `jr ra; nop` stub per entry so the decompiler shows calls by native name
        mem.createInitializedBlock("natives", toAddr(0x400000), code - 0x400000, (byte) 0, monitor, false).setExecute(true);

        Register gpReg = currentProgram.getRegister("gp");
        if (gpReg != null)
            currentProgram.getProgramContext().setValue(gpReg, toAddr(code), toAddr(code + text - 1), BigInteger.valueOf(gp));

        SymbolTable st = currentProgram.getSymbolTable();
        int nf = 0, nc = 0, nr = 0;
        for (String[] f : rows) {
            switch (f[0]) {
                case "NATIVE" -> {
                    Address a = toAddr(hex(f[1]));
                    mem.setInt(a, 0x03e00008);
                    mem.setInt(a.add(4), 0);
                    new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                    createFunction(a, f[2]);
                }
                case "LABEL" -> st.createLabel(toAddr(hex(f[1])), f[2], SourceType.USER_DEFINED);
                case "FUNC" -> {
                    Address a = toAddr(hex(f[1]));
                    new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                    Function fn = getFunctionAt(a);
                    if (fn == null) fn = createFunction(a, f[2]);
                    else fn.setName(f[2], SourceType.USER_DEFINED);
                    if (fn != null) nf++;
                }
                default -> { }
            }
        }
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (String[] f : rows) {
            switch (f[0]) {
                case "PLATE" -> setPlateComment(toAddr(hex(f[1])), f[2]);
                case "CMT" -> { setEOLComment(toAddr(hex(f[1])), f[2]); nc++; }
                case "REF" -> {
                    Address from = toAddr(hex(f[1]));
                    Instruction ins = getInstructionAt(from);
                    if (ins == null) break;
                    int op = ins.getNumOperands() - 1;
                    boolean have = false;
                    for (Reference x : ins.getOperandReferences(op)) have |= x.getToAddress().getOffset() == hex(f[2]);
                    if (have) break;
                    RefType t = f[3].equals("W") ? RefType.WRITE : f[3].equals("R") ? RefType.READ : RefType.DATA;
                    rm.addMemoryReference(from, toAddr(hex(f[2])), t, SourceType.USER_DEFINED, op);
                    nr++;
                }
                default -> { }
            }
        }
        println(String.format("%s: %d functions, %d native-call comments, %d data refs", mod[1], nf, nc, nr));
    }
}
