// Phase 2 step 2: attribute .text functions to source files (translation units) and tag them in Ghidra.
//
// Evidence, strongest first:
//   direct  the function references a "<dir>/<file>.cpp" string (__FILE__ passed to an assert handler or
//           debug allocator). __FILE__ inside a header gives the header path, so a .cpp path is always own-TU.
//   run     unreferenced functions lying between two functions of the same TU, with no other TU in between
//           (gcc emits a TU's functions contiguously, and ld lays TUs out in link order).
//   rodata  every .rodata address the function references lies inside exactly one TU's rodata span
//           (span = min..max .rodata address referenced by that TU's direct/run functions).
// Functions get Ghidra function tags "src:<path>" (direct) or "src~<method>:<path>" (inferred).
// Linkonce (.gnu.linkonce.t.*) functions are inline/template code and are not attributed.
//
// Args: <outDir>  writes funcs.tsv (one row per .text function) and rodata.tsv (TU rodata spans).
//@category RuleOfRose

import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

public class RoRSourceFiles extends GhidraScript {

    static final String PATH_RE = "[A-Za-z0-9_][A-Za-z0-9_./-]*\\.(cpp|c|cc|h|hpp)";

    static class Fn {
        Function f; Set<String> cpp = new TreeSet<>(), hdr = new TreeSet<>(); List<Long> ro = new ArrayList<>();
        String file, method = "none";
    }

    Memory mem;

    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : null;
        mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text"), ro = mem.getBlock(".rodata");

        // path strings in .rodata
        Map<Address, String> paths = new HashMap<>();
        for (Address a = ro.getStart(); a.compareTo(ro.getEnd()) < 0; ) {
            Address s = a;
            StringBuilder sb = new StringBuilder();
            int c;
            while (a.compareTo(ro.getEnd()) < 0 && (c = mem.getByte(a) & 0xff) != 0 && c >= 0x20 && c < 0x7f) { sb.append((char) c); a = a.add(1); }
            a = a.add(1);
            if (sb.toString().matches(PATH_RE)) paths.put(s, sb.toString());
        }

        // per-function references
        List<Fn> fns = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(new AddressSet(text.getStart(), text.getEnd()), true)) {
            Fn x = new Fn(); x.f = f;
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                for (Reference r : ins.getReferencesFrom()) {
                    Address t = r.getToAddress();
                    if (!ro.contains(t)) continue;
                    // Ghidra's propagation leaves references to a lui's bare upper half (0x300000 etc.) on later
                    // instructions; a 64K-aligned target is treated as one of those, not as a real data reference.
                    if ((t.getOffset() & 0xffff) == 0) continue;
                    String p = paths.get(t);
                    if (p != null) { if (p.endsWith(".h") || p.endsWith(".hpp")) x.hdr.add(p); else x.cpp.add(p); }
                    x.ro.add(t.getOffset());
                }
            }
            if (x.cpp.size() == 1) { x.file = x.cpp.iterator().next(); x.method = "direct"; }
            else if (x.cpp.size() > 1) x.method = "conflict";
            fns.add(x);
        }

        // runs between same-file direct functions
        fillRuns(fns, "direct", "run");

        // rodata spans per TU, then attribute by rodata
        Map<String, long[]> span = spans(fns);
        if (getScriptArgs().length > 1 && getScriptArgs()[1].equals("debug"))
            for (Map.Entry<String, long[]> e : span.entrySet()) println("SPAN " + e.getKey() + " " + Long.toHexString(e.getValue()[0]) + "-" + Long.toHexString(e.getValue()[1]));
        // code window per file: strictly between the code of the files before/after it in rodata (link) order
        Map<String, long[]> code = codeExtents(fns);
        List<String> order = new ArrayList<>(span.keySet());
        order.sort(Comparator.comparingLong(f -> anchors.get(f)));
        Map<String, long[]> window = new HashMap<>();
        for (int i = 0; i < order.size(); i++) {
            long lo = i == 0 ? 0 : code.get(order.get(i - 1))[1];
            long hi = i == order.size() - 1 ? Long.MAX_VALUE : code.get(order.get(i + 1))[0];
            window.put(order.get(i), new long[] {lo, hi});
        }
        int byRo = 0;
        for (Fn x : fns) {
            if (x.file != null || x.ro.isEmpty() || x.method.equals("conflict")) continue;
            String hit = null; boolean ambiguous = false;
            for (long a : x.ro) {
                String in = null;
                for (Map.Entry<String, long[]> e : span.entrySet())
                    if (a >= e.getValue()[0] && a <= e.getValue()[1]) { if (in != null && !in.equals(e.getKey())) { ambiguous = true; } in = e.getKey(); }
                if (in == null) { ambiguous = true; break; }
                if (hit == null) hit = in; else if (!hit.equals(in)) ambiguous = true;
            }
            if (ambiguous || hit == null) continue;
            long ep = x.f.getEntryPoint().getOffset();
            if (ep <= window.get(hit)[0] || ep >= window.get(hit)[1]) continue;
            x.file = hit; x.method = "rodata"; byRo++;
        }
        // runs again, now that rodata-attributed functions can close gaps
        fillRuns(fns, null, "run");
        extendByClass(fns);

        // tags
        FunctionTagManager tm = currentProgram.getFunctionManager().getFunctionTagManager();
        int tagged = 0;
        for (Fn x : fns) {
            for (FunctionTag t : new ArrayList<>(x.f.getTags())) if (t.getName().startsWith("src")) x.f.removeTag(t.getName());
            if (x.file == null) continue;
            x.f.addTag(x.method.equals("direct") ? "src:" + x.file : "src~" + x.method + ":" + x.file);
            tagged++;
        }
        Map<String, Integer> counts = new TreeMap<>();
        for (Fn x : fns) counts.merge(x.method, 1, Integer::sum);
        println("functions in .text=" + fns.size() + " " + counts + " tagged=" + tagged + " files=" + span.size());

        if (outDir != null) {
            File d = new File(outDir); d.mkdirs();
            try (PrintWriter w = new PrintWriter(new File(d, "funcs.tsv"), "UTF-8")) {
                w.println("start\tend\tname\tfile\tmethod\tcpp_refs\theader_refs");
                for (Fn x : fns)
                    w.println(x.f.getEntryPoint() + "\t" + x.f.getBody().getMaxAddress() + "\t" + x.f.getName(true) + "\t" + (x.file == null ? "" : x.file)
                        + "\t" + x.method + "\t" + String.join(";", x.cpp) + "\t" + String.join(";", x.hdr));
            }
            try (PrintWriter w = new PrintWriter(new File(d, "rodata.tsv"), "UTF-8")) {
                w.println("file\trodata_lo\trodata_hi");
                for (Map.Entry<String, long[]> e : spans(fns).entrySet())
                    w.println(e.getKey() + "\t" + Long.toHexString(e.getValue()[0]) + "\t" + Long.toHexString(e.getValue()[1]));
            }
            println("reports written to " + d);
        }
    }

    // An unattributed run of functions bounded on both sides by the same file (by functions whose method is
    // `seed`, or any attributed function when seed is null) is attributed to that file.
    void fillRuns(List<Fn> fns, String seed, String method) {
        int last = -1;
        for (int i = 0; i < fns.size(); i++) {
            Fn x = fns.get(i);
            if (x.file == null || (seed != null && !x.method.equals(seed))) {
                if (x.method.equals("conflict") || (seed != null && x.file != null)) last = -1;
                continue;
            }
            if (last >= 0 && fns.get(last).file.equals(x.file)) {
                boolean clean = true;
                for (int k = last + 1; k < i; k++) if (fns.get(k).file != null || fns.get(k).method.equals("conflict")) { clean = false; break; }
                if (clean) for (int k = last + 1; k < i; k++) { fns.get(k).file = x.file; fns.get(k).method = method; }
            }
            last = i;
        }
    }

    Map<String, long[]> codeExtents(List<Fn> fns) {
        Map<String, long[]> code = new HashMap<>();
        for (Fn x : fns) {
            if (x.file == null) continue;
            long a = x.f.getEntryPoint().getOffset();
            long[] c = code.computeIfAbsent(x.file, k -> new long[] {Long.MAX_VALUE, Long.MIN_VALUE});
            c[0] = Math.min(c[0], a); c[1] = Math.max(c[1], a);
        }
        return code;
    }

    // Grow each attributed segment outward over unattributed neighbours that are methods of a class whose
    // attributed methods all sit in that one file.
    void extendByClass(List<Fn> fns) {
        Map<String, Set<String>> classFiles = new HashMap<>();
        for (Fn x : fns) {
            String c = cls(x);
            if (c != null && x.file != null) classFiles.computeIfAbsent(c, k -> new HashSet<>()).add(x.file);
        }
        boolean changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < fns.size(); i++) {
                Fn x = fns.get(i);
                if (x.file != null || x.method.equals("conflict")) continue;
                String c = cls(x);
                Set<String> files = c == null ? null : classFiles.get(c);
                if (files == null || files.size() != 1) continue;
                String file = files.iterator().next();
                boolean adj = (i > 0 && file.equals(fns.get(i - 1).file)) || (i + 1 < fns.size() && file.equals(fns.get(i + 1).file));
                if (adj) { x.file = file; x.method = "class"; changed = true; }
            }
        }
    }

    String cls(Fn x) {
        Namespace n = x.f.getParentNamespace();
        return n == null || n.isGlobal() ? null : n.getName(true);
    }

    // ld lays out each TU's .rodata in link order, and a TU's own .cpp path string lives in its .rodata. So a TU's
    // span is the min..max of its functions' rodata references that fall strictly between the previous and next
    // TU's path strings (anchors); references outside that window (shared tables, artefacts) are ignored.
    Map<String, Long> anchors = new HashMap<>();

    Map<String, long[]> spans(List<Fn> fns) {
        Map<String, TreeSet<Long>> refs = new TreeMap<>();
        for (Fn x : fns) {
            if (x.file == null || x.method.equals("rodata")) continue;
            refs.computeIfAbsent(x.file, k -> new TreeSet<>()).addAll(x.ro);
            if (x.method.equals("direct"))
                for (long a : x.ro) if (x.file.equals(pathAt(a))) anchors.putIfAbsent(x.file, a);
        }
        TreeSet<Long> all = new TreeSet<>(anchors.values());
        Map<String, long[]> span = new TreeMap<>();
        for (Map.Entry<String, TreeSet<Long>> e : refs.entrySet()) {
            Long anchor = anchors.get(e.getKey());
            if (anchor == null) continue;
            Long prev = all.lower(anchor), next = all.higher(anchor);
            long wlo = prev == null ? 0 : prev + 1, whi = next == null ? Long.MAX_VALUE : next - 1;
            SortedSet<Long> in = e.getValue().subSet(wlo, true, whi, true);
            span.put(e.getKey(), new long[] {Math.min(anchor, in.isEmpty() ? anchor : in.first()), Math.max(anchor, in.isEmpty() ? anchor : in.last())});
        }
        return span;
    }

    String pathAt(long a) {
        try {
            StringBuilder sb = new StringBuilder();
            Address p = toAddr(a);
            for (int i = 0, c; i < 128 && (c = mem.getByte(p.add(i)) & 0xff) != 0; i++) sb.append((char) c);
            return sb.toString();
        } catch (Exception e) { return null; }
    }
}
