// Creates the functions Ghidra never found because only data or a code-built constant points at them: aligned words in initialized
// non-executable blocks, lui/addiu (ori) pairs and j/jal targets, whose value is an aligned address in EE code (not .vutext) that
// no function contains.
// Typical cases: two-instruction leaves in g++ 2.9x vtables ({0xffff0000, pfn} pairs), the script VM's default
// natives (vm_native_table 0x2eed90), i3r node handlers (table at 0x2f12f0). Without them PS2Recomp has no function
// at an indirect-call target and parks the thread (milestone 3, jalr 0x22e450 -> 0x22cd20).
//
// Jump tables are not function-pointer tables: a run of consecutive code-pointer words that contains any switch
// case (a Ghidra computed-jump target, or a label owned by a function/switch as RoRJumpTables.java leaves them) is
// a jump table, and none of its targets becomes a function; functions this script created at such targets earlier
// are removed. Zero words at the target (padding) are skipped. Idempotent.
//
// Args: [report.tsv]   target, first referencing word, result
//@category RuleOfRose

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class RoRFunctionPointers extends GhidraScript {

    private static final String MARK = "RoR: created by RoRFunctionPointers.java";

    @Override
    public void run() throws Exception {
        int split = splitDetachedChunks();

        // Switch cases Ghidra knows, and the entries of our overrides (data/jumptables.tsv: function, table, count).
        Set<Long> cases = new HashSet<>();
        for (Instruction in : currentProgram.getListing().getInstructions(true))
            for (Reference r : in.getReferencesFrom())
                if (r.getReferenceType().isComputed() && r.getReferenceType().isJump()) cases.add(r.getToAddress().getOffset());
        File tsv = new File(new File(getSourceFile().getParentFile().getFile(false), "data"), "jumptables.tsv");
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(tsv), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] c = line.split("\t");
                Address table = toAddr(c[1]);
                for (int i = 0, n = Integer.decode(c[2]); i < n; i++) cases.add(getInt(table.add(4L * i)) & 0xffffffffL);
            }
        }

        // Runs of code-pointer words in data.
        Set<Long> switchTargets = new HashSet<>();
        TreeMap<Long, Long> candidates = new TreeMap<>();  // target -> first referencing word
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            // Only the ELF's own data (the EE extension maps low memory blocks with unrelated contents).
            if (!b.isInitialized() || b.isExecute() || b.getStart().getOffset() < 0x100000L) continue;
            List<long[]> run = new ArrayList<>();  // {word address, target}
            for (Address a = b.getStart(); ; a = a.add(4)) {
                boolean end = a.add(3).compareTo(b.getEnd()) > 0;
                Address t = end ? null : codePointer(getInt(a) & 0xffffffffL);
                if (t != null) {
                    run.add(new long[] {a.getOffset(), t.getOffset()});
                    continue;
                }
                boolean table = false;
                for (long[] w : run) table |= cases.contains(w[1]) || isCaseLabel(toAddr(w[1]));
                for (long[] w : run) {
                    if (table) switchTargets.add(w[1]);
                    else if (getFunctionContaining(toAddr(w[1])) == null || isMine(w[1])) candidates.putIfAbsent(w[1], w[0]);
                }
                run.clear();
                if (end) break;
            }
        }

        // Addresses built in code (lui r, hi; then within 5 instructions addiu/ori r, r, lo): callbacks and handlers
        // passed as arguments (libmc RPC end callback 0x24fc60, DMAC 3 handler 0x252ea8, i3r handlers ...).
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.isInitialized() || !b.isExecute() || b.getName().equals(".vutext") || b.getStart().getOffset() < 0x100000L) continue;
            for (Address a = b.getStart(); a.add(3).compareTo(b.getEnd()) <= 0; a = a.add(4)) {
                long w = getInt(a) & 0xffffffffL;
                if ((w >>> 26) == 2 || (w >>> 26) == 3) {
                    // j / jal: a tail jump or call to code outside every function (j 0x1bf078 from
                    // MagLifeMenuSelLang::isInit 0x285eec reaches an 8-byte leaf between two functions).
                    long v = ((a.getOffset() + 4) & 0xf0000000L) | ((w & 0x3ffffff) << 2);
                    Address t = codePointer(v);
                    if (t != null && getFunctionContaining(a) != null && !switchTargets.contains(v) && !cases.contains(v)
                            && !isCaseLabel(t) && (getFunctionContaining(t) == null || isMine(v)))
                        candidates.putIfAbsent(v, a.getOffset());
                    continue;
                }
                if ((w >>> 26) != 15) continue;
                long rt = (w >>> 16) & 31;
                for (int k = 1; k <= 5; k++) {
                    if (a.add(4L * k + 3).compareTo(b.getEnd()) > 0) break;
                    long n = getInt(a.add(4L * k)) & 0xffffffffL;
                    long op = n >>> 26;
                    if (((n >>> 21) & 31) != rt || ((n >>> 16) & 31) != rt || (op != 9 && op != 13)) continue;
                    long low = n & 0xffff;
                    if (op == 9 && (low & 0x8000) != 0) low -= 0x10000;
                    long v = (((w & 0xffff) << 16) + low) & 0xffffffffL;
                    Address t = codePointer(v);
                    if (t != null && !switchTargets.contains(v) && !cases.contains(v) && !isCaseLabel(t)
                            && (getFunctionContaining(t) == null || isMine(v)))
                        candidates.putIfAbsent(v, a.getOffset());
                    break;
                }
            }
        }

        // Functions created earlier that are no longer justified: at a jump-table target, or no reference left.
        int removed = 0;
        List<Long> mine = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true))
            if (isMine(f.getEntryPoint().getOffset())) mine.add(f.getEntryPoint().getOffset());
        for (long t : mine) {
            if (switchTargets.contains(t) || !candidates.containsKey(t)) {
                currentProgram.getFunctionManager().removeFunction(toAddr(t));
                setPlateComment(toAddr(t), null);
                candidates.remove(t);
                removed++;
            }
        }

        int created = 0, kept = 0, padding = 0, failed = 0;
        StringBuilder report = new StringBuilder("# target\tref\tresult\n");
        for (Map.Entry<Long, Long> e : candidates.entrySet()) {
            Address t = toAddr(e.getKey());
            String result;
            if (switchTargets.contains(e.getKey())) {
                result = "skipped: also a jump-table target";
            } else if (isMine(e.getKey())) {
                kept++;
                result = "kept";
            } else if (getFunctionContaining(t) != null) {
                result = "covered by an earlier one";
            } else if (getInt(t) == 0) {
                padding++;
                result = "skipped: zero word";
            } else {
                if (getInstructionAt(t) == null) new DisassembleCommand(t, null, true).applyTo(currentProgram, monitor);
                CreateFunctionCmd cmd = new CreateFunctionCmd(t);
                if (getInstructionAt(t) != null && cmd.applyTo(currentProgram, monitor)) {
                    created++;
                    result = "created";
                    setPlateComment(t, MARK + ": data word at " + String.format("0x%08x", e.getValue()) + " points here");
                } else {
                    failed++;
                    result = "failed: " + cmd.getStatusMsg();
                }
            }
            report.append(String.format("0x%08x\t0x%08x\t%s%n", e.getKey(), e.getValue(), result));
        }
        if (getScriptArgs().length > 0) {
            try (Writer w = new OutputStreamWriter(new FileOutputStream(getScriptArgs()[0]), StandardCharsets.UTF_8)) {
                w.write(report.toString());
            }
        }
        println(String.format("detached chunks split off %d; function-pointer targets %d: created %d, kept %d, padding %d, "
                + "failed %d; jump-table targets %d, wrongly created functions removed %d",
                split, candidates.size(), created, kept, padding, failed, switchTargets.size(), removed));
    }

    // PS2Recomp decodes a function as [entry, body max + 1). A body range below the entry, in another memory block
    // or past the next function's entry is a separate function that Ghidra merged because it is reached by a tail
    // jump (MagLifeMenu*::isInit jump to 8-byte leaves in .text; 0x233d98 jumps into a whole function at 0x233870).
    // Such ranges leave the body and become functions of their own.
    private int splitDetachedChunks() throws Exception {
        int n = 0;
        List<Function> all = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) all.add(f);
        for (Function f : all) {
            AddressSetView body = f.getBody();
            Address entry = f.getEntryPoint();
            if (body.getNumAddressRanges() < 2 && body.getMinAddress().compareTo(entry) >= 0) continue;
            MemoryBlock entryBlock = currentProgram.getMemory().getBlock(entry);
            Function next = getFunctionAfter(entry);
            AddressSet detached = new AddressSet();
            // also the part of a contiguous range below the entry (FUN_0021a374 starts with the leaf j'd to at 0x21a348)
            if (body.getMinAddress().compareTo(entry) < 0)
                detached.add(body.intersectRange(body.getMinAddress(), entry.previous()));
            for (AddressRange r : body.getAddressRanges()) {
                if (r.contains(entry)) continue;
                Address lo = r.getMinAddress();
                if (lo.compareTo(entry) < 0 || currentProgram.getMemory().getBlock(lo) != entryBlock
                        || (next != null && lo.compareTo(next.getEntryPoint()) >= 0))
                    detached.add(r);
            }
            if (detached.isEmpty()) continue;
            f.setBody(body.subtract(detached));
            for (AddressRange r : detached.getAddressRanges()) {
                Address s = r.getMinAddress();
                if (getFunctionContaining(s) != null) continue;
                if (new CreateFunctionCmd(s).applyTo(currentProgram, monitor)) {
                    setPlateComment(s, "RoR: split from " + f.getName(true) + " by RoRFunctionPointers.java (reached by a tail jump)");
                    n++;
                }
            }
        }
        return n;
    }

    private Address codePointer(long w) {
        if ((w & 3) != 0 || w == 0) return null;
        Address t;
        try { t = toAddr(w); } catch (Exception e) { return null; }
        MemoryBlock tb = currentProgram.getMemory().getBlock(t);
        if (tb == null || !tb.isExecute() || !tb.isInitialized() || tb.getName().equals(".vutext")) return null;
        return t;
    }

    private boolean isMine(long t) {
        Function f = getFunctionAt(toAddr(t));
        String c = f == null ? null : getPlateComment(toAddr(t));
        return c != null && c.startsWith(MARK);
    }

    // Switch cases of data/jumptables.tsv overrides (outside the function body) carry a label owned by their
    // function or switch.
    private boolean isCaseLabel(Address t) {
        for (Symbol s : currentProgram.getSymbolTable().getSymbols(t))
            if (!s.getParentNamespace().isGlobal()) return true;
        return false;
    }
}
