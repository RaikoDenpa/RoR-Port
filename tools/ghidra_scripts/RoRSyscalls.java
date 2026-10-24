// Phase 2 step 3a: name libkernl syscall wrappers.
//
// A Sony libkernl wrapper is `li v1,<n>; syscall; jr ra; nop` (v1 = syscall number, negative for i* variants).
// Every `syscall` in .text is found, v1 is resolved by straight-line constant tracking inside its function, and
// functions of at most MAX_WRAPPER_INSNS instructions with exactly one syscall are named from data/ee_syscalls.tsv.
// Larger functions that issue syscalls inline are only reported.
//
// Args: [outFile]  optional TSV report (address, function, size, number, table name, action)
//@category RuleOfRose

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.SourceType;

public class RoRSyscalls extends GhidraScript {

    static final int MAX_WRAPPER_INSNS = 6;

    @Override
    public void run() throws Exception {
        File table = new File(getSourceFile().getParentFile().getFile(false), "data/ee_syscalls.tsv");
        Map<Long, String> names = new HashMap<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(table), StandardCharsets.UTF_8))) {
            for (String l; (l = r.readLine()) != null; ) {
                if (l.startsWith("#") || l.startsWith("number")) continue;
                String[] c = l.split("\t");
                names.put(Long.parseLong(c[0]), c[1]);
            }
        }
        PrintWriter out = getScriptArgs().length > 0 ? new PrintWriter(new File(getScriptArgs()[0]), "UTF-8") : null;
        if (out != null) out.println("syscall_at\tfunction(name before, source)\tinsns\tnumber\ttable_name\taction");

        Map<String, Integer> actions = new TreeMap<>();
        Set<Function> seen = new HashSet<>();
        AddressSetView text = currentProgram.getMemory().getBlock(".text").getAddressRange().getMinAddress() == null ? null
            : new AddressSet(currentProgram.getMemory().getBlock(".text").getStart(), currentProgram.getMemory().getBlock(".text").getEnd());
        for (Instruction ins : currentProgram.getListing().getInstructions(text, true)) {
            if (!mnem(ins).equals("syscall")) continue;
            Function f = getFunctionContaining(ins.getAddress());
            String before = f == null ? "" : f.getName() + "(" + f.getSymbol().getSource() + ")";
            Long n = f == null ? null : v1At(f, ins.getAddress());
            String nm = n == null ? null : names.get(n);
            long count = f == null ? 0 : countInsns(f);
            int syscalls = f == null ? 0 : countSyscalls(f);
            String action;
            if (f == null) action = "no-function";
            else if (n == null) action = "v1-unknown";
            else if (nm == null) action = "number-not-in-table";
            else if (count > MAX_WRAPPER_INSNS || syscalls != 1) action = "inline-not-renamed";
            else {
                // The EE extension's analyzer already names many wrappers (often with Sony's leading-underscore
                // names, e.g. _Exit/_StartThread); keep those, but replace its RFU### placeholders.
                boolean keep = f.getSymbol().getSource() != SourceType.DEFAULT && !f.getName().startsWith("RFU");
                action = keep ? (f.getName().equals(nm) ? "named" : "kept-extension-name") : "named";
                if (seen.add(f)) {
                    String finalName = keep ? f.getName() : nm;
                    f.getSymbol().setNameAndNamespace(finalName, currentProgram.getGlobalNamespace(), SourceType.USER_DEFINED);
                    setPlateComment(f.getEntryPoint(), "RoR: libkernl syscall wrapper, v1=" + n + " (0x" + Long.toHexString(n) + "); ps2sdk name "
                        + nm + (keep && !finalName.equals(nm) ? ", EE-extension name kept" : "") + " [data/ee_syscalls.tsv]");
                }
            }
            actions.merge(action, 1, Integer::sum);
            if (out != null) out.println(ins.getAddress() + "\t" + (f == null ? "" : f.getEntryPoint() + " " + before) + "\t" + count + "\t" + n + "\t" + nm + "\t" + action);
        }
        if (out != null) out.close();
        println("syscall sites: " + actions + " wrappers named=" + seen.size());
    }

    long countInsns(Function f) { long c = 0; for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) c++; return c; }

    int countSyscalls(Function f) { int c = 0; for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) if (mnem(i).equals("syscall")) c++; return c; }

    // value of v1 at `at`, tracking li/lui/addiu/ori/move in address order from the function entry
    Long v1At(Function f, Address at) {
        Map<String, Long> regs = new HashMap<>();
        for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            if (i.getAddress().equals(at)) return regs.get("v1");
            String m = mnem(i);
            String d = reg(i, 0);
            if (d == null || m.startsWith("s") && !m.startsWith("sl") && !m.startsWith("sr") && !m.startsWith("su") || m.startsWith("b") || m.startsWith("j")) continue;
            Long v = null;
            switch (m) {
                case "li": v = sval(i, 1); break;
                case "lui": v = (uval(i, 1) << 16); break;
                case "addiu": { Long s = src(i, 1, regs); if (s != null) v = s + sval(i, 2); break; }
                case "ori": { Long s = src(i, 1, regs); if (s != null) v = s | uval(i, 2); break; }
                case "move": v = src(i, 1, regs); break;
                default: break;
            }
            if (v == null) regs.remove(d); else regs.put(d, (long) (int) (long) v);
        }
        return null;
    }

    String mnem(Instruction i) { String m = i.getMnemonicString(); return m.startsWith("_") ? m.substring(1) : m; }
    long sval(Instruction i, int k) { Scalar s = i.getScalar(k); return s == null ? 0 : s.getSignedValue(); }
    long uval(Instruction i, int k) { Scalar s = i.getScalar(k); return s == null ? 0 : s.getUnsignedValue(); }
    Long src(Instruction i, int k, Map<String, Long> regs) {
        String r = reg(i, k);
        if (r == null) return null;
        if (r.equals("zero")) return Long.valueOf(0);
        return regs.get(r);
    }

    // R5900 operands come back as sub-registers (v1_lo); use the base register name
    String reg(Instruction i, int k) {
        if (k >= i.getNumOperands()) return null;
        for (Object o : i.getOpObjects(k)) {
            if (o instanceof Register) {
                Register r = (Register) o;
                while (r.getParentRegister() != null && r.getName().contains("_")) r = r.getParentRegister();
                return r.getName();
            }
        }
        return null;
    }
}
