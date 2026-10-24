// Phase 2 step 1: recover the C++ class hierarchy of SLES_542.18 (g++ 2.9x, v2 ABI) and name it.
//
//  1. Every .gnu.linkonce.t.<mangled> block becomes a function named by demangling its section name;
//     every .gnu.linkonce.d._vt$<class> block is labelled <Class>::vtable.
//  2. libgcc RTTI helpers (__rtti_user / __rtti_si / __rtti_class) are found as the calls that linkonce
//     __tf functions make with their mangled class-name string in a1. Every caller of those helpers is a
//     type_info function; its a0/a1/a2/a3 give the __ti node, class name and base type_info(s).
//  3. Vtables are found by scanning data for {0, __tf<class>} (entry 0 of a g++ v2 vtable); entries are
//     8 bytes {int16 delta, int16 index, pfn}, ending with {0, 0}. Unnamed functions in a vtable slot are
//     named <Owner>::vfunc_<slot>, where Owner is the most basic class in the chain using that pfn there.
//
// Args: <outDir>  TSV reports (classes.tsv, vtables.tsv, unresolved.tsv) are written there.
// Re-runnable: names are set with USER_DEFINED source and existing user names are overwritten.
//@category RuleOfRose

import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.demangler.*;
import ghidra.app.util.demangler.gnu.*;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class RoRClasses extends GhidraScript {

    static final String LT = ".gnu.linkonce.t.", LD = ".gnu.linkonce.d.";
    static final Set<String> STORES = new HashSet<>(Arrays.asList(
        "sb", "sh", "sw", "sd", "sq", "swl", "swr", "sdl", "sdr", "swc1", "sdc1", "sqc2", "sc", "scd"));

    GnuDemangler demangler = new GnuDemangler();
    GnuDemanglerOptions dopts;
    Memory mem;

    static class Call { Address site, target; Map<String, Long> regs; }
    static class Cls {
        String name, mangled; Address tf, ti; String kind = "?";
        List<String> bases = new ArrayList<>(); List<Address> vtables = new ArrayList<>();
        String primaryBase;
    }
    static class Slot { int index; long deltaWord; Address pfn; }

    Map<Address, Cls> byTi = new LinkedHashMap<>(), byTf = new LinkedHashMap<>();
    Map<String, Cls> byName = new LinkedHashMap<>();
    List<String> unresolved = new ArrayList<>();

    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : null;
        mem = currentProgram.getMemory();
        dopts = new GnuDemanglerOptions(GnuDemanglerFormat.GNU, true);
        dopts.setApplySignature(false);
        dopts.setApplyCallingConvention(false);
        dopts.setDoDisassembly(true);
        dopts.setDemangleOnlyKnownPatterns(false);

        // --- 1. linkonce sections ---
        int nt = 0, nd = 0, bad = 0;
        List<Address> linkonceTf = new ArrayList<>();
        for (MemoryBlock b : mem.getBlocks()) {
            String n = b.getName();
            if (n.startsWith(LT)) {
                String m = n.substring(LT.length());
                Address a = b.getStart();
                ensureFunction(a);
                if (!applyDemangled(m, a)) { bad++; unresolved.add("demangle\t" + a + "\t" + m); }
                if (m.startsWith("__tf")) linkonceTf.add(a);
                nt++;
            } else if (n.startsWith(LD)) {
                String m = n.substring(LD.length());
                if (m.startsWith("_vt$")) setName(b.getStart(), ns(className(m.substring(4))), "vtable");
                else if (!applyDemangled(m, b.getStart())) { bad++; unresolved.add("demangle\t" + b.getStart() + "\t" + m); }
                nd++;
            }
        }
        println("linkonce text=" + nt + " data=" + nd + " demangle failures=" + bad);

        // --- 2. RTTI helpers and type_info functions ---
        Map<Address, Integer> helperVotes = new HashMap<>();
        for (Address tf : linkonceTf) {
            Function f = getFunctionAt(tf);
            if (f == null) continue;
            for (Call c : calls(f)) {
                Long a1 = c.regs.get("a1");
                if (a1 != null && isMangledClassString(a1)) helperVotes.merge(c.target, 1, Integer::sum);
            }
        }
        Set<Address> helpers = helperVotes.keySet();
        println("rtti helpers: " + helperVotes);

        Map<Address, List<Call>> helperCalls = new HashMap<>();
        Set<Function> tfFuncs = new LinkedHashSet<>();
        for (Address h : helpers) {
            for (Reference r : getReferencesTo(h)) {
                if (!r.getReferenceType().isCall()) continue;
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) tfFuncs.add(f);
            }
        }
        for (Function f : tfFuncs) {
            for (Call c : calls(f)) {
                if (!helpers.contains(c.target)) continue;
                helperCalls.computeIfAbsent(c.target, k -> new ArrayList<>()).add(c);
                Long a0 = c.regs.get("a0"), a1 = c.regs.get("a1");
                if (a0 == null || a1 == null || !isMangledClassString(a1)) {
                    unresolved.add("rtti-call\t" + c.site + "\ta0=" + a0 + " a1=" + a1);
                    continue;
                }
                Cls k = new Cls();
                k.mangled = readCString(toAddr(a1));
                k.name = className(k.mangled);
                k.tf = f.getEntryPoint();
                k.ti = toAddr(a0);
                byTi.put(k.ti, k); byTf.put(k.tf, k); byName.put(k.name, k);
            }
        }
        // classify helpers by argument shape, then resolve bases
        Map<Address, String> helperKind = new HashMap<>();
        for (Map.Entry<Address, List<Call>> e : helperCalls.entrySet()) {
            int withTiA2 = 0, withCount = 0;
            for (Call c : e.getValue()) {
                Long a2 = c.regs.get("a2"), a3 = c.regs.get("a3");
                if (a2 != null && byTi.containsKey(toAddr(a2))) withTiA2++;
                if (a3 != null && a3 > 0 && a3 < 32 && a2 != null) withCount++;
            }
            int n = e.getValue().size();
            String kind = withTiA2 == n ? "si" : (withCount == n ? "class" : (withTiA2 == 0 && withCount == 0 ? "user" : "mixed"));
            helperKind.put(e.getKey(), kind);
            println("helper " + e.getKey() + " calls=" + n + " a2=ti:" + withTiA2 + " a3=count:" + withCount + " => __rtti_" + kind);
            if (!kind.equals("mixed")) setName(e.getKey(), null, "__rtti_" + kind);
        }
        for (Map.Entry<Address, List<Call>> e : helperCalls.entrySet()) {
            String kind = helperKind.get(e.getKey());
            for (Call c : e.getValue()) {
                Long a0 = c.regs.get("a0");
                if (a0 == null) continue;
                Cls k = byTi.get(toAddr(a0));
                if (k == null) continue;
                k.kind = kind;
                if (kind.equals("si")) {
                    Cls b = byTi.get(toAddr(c.regs.get("a2")));
                    k.bases.add(b.name + "@0");
                    k.primaryBase = b.name;
                } else if (kind.equals("class")) {
                    Address arr = toAddr(c.regs.get("a2"));
                    long cnt = c.regs.get("a3");
                    for (int i = 0; i < cnt; i++) {
                        Address ent = arr.add(i * 8L);
                        long bti = u32(ent), bits = u32(ent.add(4));
                        Cls b = byTi.get(toAddr(bti));
                        long offset = bits & 0x1fffffffL; boolean virt = ((bits >> 29) & 1) != 0; long access = (bits >> 30) & 3;
                        String bn = b == null ? String.format("?ti@%08x", bti) : b.name;
                        k.bases.add(bn + "@" + offset + (virt ? ":virtual" : "") + ":acc" + access);
                        if (i == 0 && b != null) k.primaryBase = b.name;
                    }
                }
            }
        }
        // type_info function and node: Class::__tf / Class::__ti (the demangler's "type_info function" text is not a usable name)
        int outside = 0;
        for (Cls k : byTi.values()) {
            if (!blockName(k.tf).startsWith(LT)) { ensureFunction(k.tf); outside++; }
            setName(k.tf, ns(k.name), "__tf");
            setName(k.ti, ns(k.name), "__ti");
        }
        println("classes=" + byTi.size() + " (type_info functions outside linkonce: " + outside + ")");

        // --- 3. vtables ---
        Map<Address, List<Slot>> vtables = new TreeMap<>();
        Map<Address, Cls> vtClass = new HashMap<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b.isExecute() || b.isOverlay() || b.getName().equals(".gcc_except_table")) continue;
            if (b.getStart().getAddressSpace() != currentProgram.getAddressFactory().getDefaultAddressSpace()) continue;
            for (Address a = b.getStart(); a.add(8).compareTo(b.getEnd()) <= 0; a = a.add(4)) {
                long w0 = u32(a), w1 = u32(a.add(4));
                if (w1 == 0) continue;
                Cls k = byTf.get(toAddr(w1));
                if (k == null) continue;
                if (w0 != 0) { unresolved.add("tf-ref-nonzero-delta\t" + a + "\t" + k.name + " w0=" + Long.toHexString(w0)); continue; }
                List<Slot> slots = new ArrayList<>();
                Address e = a;
                for (int i = 0; i < 512; i++, e = e.add(8)) {
                    long d = u32(e), p = u32(e.add(4));
                    if (d == 0 && p == 0) break;
                    Slot s = new Slot(); s.index = i; s.deltaWord = d; s.pfn = toAddr(p);
                    if (i > 0 && !isExec(s.pfn)) { unresolved.add("vt-bad-pfn\t" + e + "\t" + k.name); break; }
                    slots.add(s);
                }
                vtables.put(a, slots); vtClass.put(a, k); k.vtables.add(a);
                a = e; // skip past this table
            }
        }
        for (Map.Entry<Address, List<Slot>> e : vtables.entrySet()) {
            Cls k = vtClass.get(e.getKey());
            int idx = k.vtables.indexOf(e.getKey());
            setName(e.getKey(), ns(k.name), idx == 0 ? "vtable" : "vtable$" + idx);
        }
        // slot owner = lowest common ancestor of every class whose primary vtable holds that pfn in that slot.
        // (Several abstract bases have no emitted vtable, so walking base vtables alone would break the chain.)
        int vnamed = 0, shared = 0;
        Map<Address, Set<String>> pfnUsers = new HashMap<>();
        Map<Address, Set<Integer>> pfnSlots = new HashMap<>();
        for (Map.Entry<Address, List<Slot>> e : vtables.entrySet()) {
            Cls k = vtClass.get(e.getKey());
            if (k.vtables.indexOf(e.getKey()) != 0) continue;
            for (Slot s : e.getValue()) {
                if (s.index == 0) continue;
                pfnUsers.computeIfAbsent(s.pfn, x -> new LinkedHashSet<>()).add(k.name);
                pfnSlots.computeIfAbsent(s.pfn, x -> new TreeSet<>()).add(s.index);
            }
        }
        Map<Address, String> owners = new HashMap<>();
        for (Map.Entry<Address, Set<String>> u : pfnUsers.entrySet()) {
            Set<Integer> slots = pfnSlots.get(u.getKey());
            String lca = lca(u.getValue());
            if (slots.size() != 1 || lca == null) {
                owners.put(u.getKey(), "SHARED");
                unresolved.add("vt-shared-pfn\t" + u.getKey() + "\tslots=" + slots + " users=" + u.getValue());
                continue;
            }
            owners.put(u.getKey(), lca + "#" + slots.iterator().next());
        }
        for (Map.Entry<Address, String> o : owners.entrySet()) {
            Function f = getFunctionAt(o.getKey());
            if (f == null) { ensureFunction(o.getKey()); f = getFunctionAt(o.getKey()); }
            if (o.getValue().equals("SHARED")) { shared++; continue; }
            // every non-linkonce vtable function restarts as vfunc_N; propagation then re-derives its name, so a
            // re-run gives the same result as a fresh import (and drops names a changed rule no longer supports)
            if (f != null && !blockName(o.getKey()).startsWith(LT) && !csvNamed(o.getKey())) {
                String[] p = o.getValue().split("#");
                setName(o.getKey(), ns(p[0]), "vfunc_" + p[1]);
                vnamed++;
            }
        }
        println("vtables=" + vtables.size() + " virtual functions named=" + vnamed + " shared pfns=" + shared);
        propagateOverrideNames(vtables, vtClass, owners);

        if (outDir != null) writeReports(new File(outDir), vtables, vtClass, owners);
    }

    // ---------- reports ----------
    void writeReports(File dir, Map<Address, List<Slot>> vtables, Map<Address, Cls> vtClass, Map<Address, String> owners) throws IOException {
        dir.mkdirs();
        try (PrintWriter w = new PrintWriter(new File(dir, "classes.tsv"), "UTF-8")) {
            w.println("class\tmangled\ttf\tti\trtti\tbases\tvtables");
            for (Cls k : byTi.values()) {
                StringBuilder vts = new StringBuilder();
                for (Address v : k.vtables) vts.append(vts.length() > 0 ? ";" : "").append(v);
                w.println(k.name + "\t" + k.mangled + "\t" + k.tf + "\t" + k.ti + "\t" + k.kind + "\t" + String.join(";", k.bases) + "\t" + vts);
            }
        }
        try (PrintWriter w = new PrintWriter(new File(dir, "vtables.tsv"), "UTF-8")) {
            w.println("vtable\tclass\tslot\tdelta_word\tpfn\tfunction\towner");
            for (Map.Entry<Address, List<Slot>> e : vtables.entrySet()) {
                for (Slot s : e.getValue()) {
                    Function f = getFunctionAt(s.pfn);
                    String fn = f == null ? "?" : f.getName(true);
                    String own = owners.getOrDefault(s.pfn, "");
                    w.println(e.getKey() + "\t" + vtClass.get(e.getKey()).name + "\t" + s.index + "\t" + Long.toHexString(s.deltaWord) + "\t" + s.pfn + "\t" + fn + "\t" + own);
                }
            }
        }
        try (PrintWriter w = new PrintWriter(new File(dir, "abstract_slots.tsv"), "UTF-8")) {
            w.println("rule\tclass\tslot\tname\tevidence");
            for (String a : abstractReport) w.println(a);
        }
        try (PrintWriter w = new PrintWriter(new File(dir, "unresolved.tsv"), "UTF-8")) {
            for (String u : unresolved) w.println(u);
        }
        println("reports written to " + dir + " (unresolved=" + unresolved.size() + ")");
    }

    // Overrides share a method name. Slots (C,s) are joined into groups, and a group whose direct names agree names
    // its other members; destructors become ~Owner. Joins:
    //   1. (C,s) with (A,s), A = C's nearest ancestor that has an emitted vtable, when A's vtable has slot s.
    //      Bases without a vtable (abstract, e.g. MagLife) are skipped: with single inheritance a base's vtable is a
    //      prefix of every derived vtable, so slot s of A means the same in C however many bases lie between.
    //   2. (C,s) with every (X,s) holding the same pfn (inherited implementation), except SHARED pfns (__pure_virtual).
    //   3. abstractBaseSlots(): for a base B without a vtable, all descendants' (D,s) for slots s that provably
    //      belong to B (see there).
    // A group with no direct name can still be named by the Init/Exec/Term pattern of its neighbouring slots.
    // "Direct" names are demangled linkonce names only, so a re-run computes exactly what a fresh import does.
    List<String> abstractReport = new ArrayList<>();

    void propagateOverrideNames(Map<Address, List<Slot>> vtables, Map<Address, Cls> vtClass, Map<Address, String> owners) throws Exception {
        Map<String, String> parent = new HashMap<>();
        java.util.function.Function<String, String> find = new java.util.function.Function<>() {
            public String apply(String x) {
                String p = parent.getOrDefault(x, x);
                if (p.equals(x)) return x;
                String r = this.apply(p); parent.put(x, r); return r;
            }
        };
        java.util.function.BiConsumer<String, String> union = (x, y) -> { String a = find.apply(x), b = find.apply(y); if (!a.equals(b)) parent.put(a, b); };
        Map<String, List<Slot>> prim = new HashMap<>();
        for (Map.Entry<Address, List<Slot>> e : vtables.entrySet()) {
            Cls k = vtClass.get(e.getKey());
            if (k.vtables.indexOf(e.getKey()) == 0) prim.put(k.name, e.getValue());
        }
        Map<Address, String> firstKey = new HashMap<>();
        int skippedBaseLinks = 0;
        for (Map.Entry<String, List<Slot>> e : prim.entrySet()) {
            Cls k = byName.get(e.getKey());
            String anc = vtAncestor(e.getKey(), prim);
            List<Slot> bs = anc == null ? null : prim.get(anc);
            for (Slot s : e.getValue()) {
                if (s.index == 0) continue;
                String key = e.getKey() + "#" + s.index;
                if (bs != null && s.index < bs.size()) {
                    union.accept(key, anc + "#" + s.index);
                    if (!anc.equals(k.primaryBase)) skippedBaseLinks++;
                }
                if ("SHARED".equals(owners.get(s.pfn))) continue; // e.g. __pure_virtual: links unrelated classes
                String fk = firstKey.putIfAbsent(s.pfn, key);
                if (fk != null && fk.endsWith("#" + s.index)) union.accept(key, fk);
            }
        }
        abstractBaseSlots(prim, owners, union);

        Map<String, Set<String>> groupNames = new HashMap<>();
        Map<String, List<Slot>> groupSlots = new HashMap<>();
        Map<String, List<String>> groupClasses = new HashMap<>();
        Map<String, Integer> groupSlotIndex = new HashMap<>();
        for (Map.Entry<String, List<Slot>> e : prim.entrySet()) {
            for (Slot s : e.getValue()) {
                if (s.index == 0) continue;
                String g = find.apply(e.getKey() + "#" + s.index);
                groupSlots.computeIfAbsent(g, x -> new ArrayList<>()).add(s);
                groupClasses.computeIfAbsent(g, x -> new ArrayList<>()).add(e.getKey());
                groupSlotIndex.put(g, s.index);
                String n = directName(s.pfn);
                if (n != null) groupNames.computeIfAbsent(g, x -> new TreeSet<>()).add(n);
            }
        }
        // Init/Exec/Term: name an unnamed group when every member class has the other two names in the adjacent slots
        Map<String, String> patternNames = new HashMap<>();
        for (String g : groupSlots.keySet()) {
            String n = initExecTerm(g, groupClasses.get(g), groupSlotIndex.get(g), find, groupNames);
            if (n == null) continue;
            Set<String> have = groupNames.get(g);
            if (have != null && have.size() == 1 && have.iterator().next().equalsIgnoreCase(n) && groupClasses.get(g).size() > 1)
                abstractReport.add("pattern-agrees\t" + groupClasses.get(g).size() + " classes\t" + groupSlotIndex.get(g) + "\t" + n
                    + "\tdirect name confirmed by adjacent " + neighbourText(n) + " (e.g. " + groupClasses.get(g).get(0) + ")");
            if (have == null) {
                patternNames.put(g, n);
                abstractReport.add("pattern\t" + groupClasses.get(g).size() + " classes\t" + groupSlotIndex.get(g) + "\t" + n
                    + "\tadjacent slots are named " + neighbourText(n) + " in every member class (e.g. " + groupClasses.get(g).get(0) + ")");
            }
        }
        for (Map.Entry<String, String> p : patternNames.entrySet()) groupNames.put(p.getKey(), new TreeSet<>(Collections.singleton(p.getValue())));

        int renamed = 0, conflicts = 0;
        for (Map.Entry<String, List<Slot>> g : groupSlots.entrySet()) {
            Set<String> names = groupNames.get(g.getKey());
            if (names == null) continue;
            if (names.size() != 1) { conflicts++; unresolved.add("override-name-conflict\t" + g.getKey() + "\t" + names); continue; }
            String n = names.iterator().next();
            for (Slot s : g.getValue()) {
                Function f = getFunctionAt(s.pfn);
                // linkonce functions keep their demangled names; names/*.csv names (RoRApplyNames) win over propagation
                if (f == null || blockName(s.pfn).startsWith(LT) || csvNamed(s.pfn)) continue;
                String own = owners.get(s.pfn);
                if (own == null || own.equals("SHARED")) continue;
                String ownerCls = own.split("#")[0];
                String simple = ownerCls.contains("::") ? ownerCls.substring(ownerCls.lastIndexOf("::") + 2) : ownerCls;
                String want = n.equals("~") ? "~" + simple : n;
                if (!f.getName().equals(want)) renamed++;
                setName(s.pfn, ns(ownerCls), want);
            }
        }
        println("override propagation: renamed=" + renamed + " conflicting groups=" + conflicts
            + " links past vtable-less bases=" + skippedBaseLinks + " pattern-named groups=" + patternNames.size());
    }

    // Slots of a base B without an emitted vtable. Its slot range starts after its nearest vtable'd ancestor's
    // vtable (or at 1) and must be present in every descendant. Slot s provably belongs to B when, at s or any higher
    // slot (contiguity: B's vtable is a prefix of its descendants'), either one implementation is shared by
    // descendants in two or more of B's direct-child subtrees, or the same direct name appears in two or more of
    // those subtrees. Weaker, reported separately: every descendant's vtable has the same length, across at least
    // 3 subtrees. Owned slots join all descendants' (D,s) unless their direct names disagree.
    void abstractBaseSlots(Map<String, List<Slot>> prim, Map<Address, String> owners, java.util.function.BiConsumer<String, String> union) {
        for (Cls b : byName.values()) {
            if (prim.containsKey(b.name)) continue;
            List<String[]> desc = new ArrayList<>(); // {class, child of b it descends through}
            for (String c : prim.keySet()) {
                List<String> ch = chain(c);
                int i = ch.indexOf(b.name);
                if (i > 0) desc.add(new String[] {c, ch.get(i - 1)});
            }
            if (desc.isEmpty()) continue;
            String anc = vtAncestor(b.name, prim);
            int start = anc == null ? 1 : prim.get(anc).size();
            int minLen = Integer.MAX_VALUE, maxLen = 0;
            Set<String> tops = new HashSet<>();
            for (String[] d : desc) { int l = prim.get(d[0]).size(); minLen = Math.min(minLen, l); maxLen = Math.max(maxLen, l); tops.add(d[1]); }
            int owned = start - 1;
            Map<Integer, String> evidence = new TreeMap<>();
            for (int s = start; s < minLen; s++) {
                Map<Address, Set<String>> pf = new HashMap<>();
                Map<String, Set<String>> nm = new TreeMap<>();
                for (String[] d : desc) {
                    Slot sl = prim.get(d[0]).get(s);
                    if (!"SHARED".equals(owners.get(sl.pfn))) pf.computeIfAbsent(sl.pfn, x -> new HashSet<>()).add(d[1]);
                    String n = directName(sl.pfn);
                    if (n != null) nm.computeIfAbsent(n, x -> new HashSet<>()).add(d[1]);
                }
                List<String> ev = new ArrayList<>();
                for (Map.Entry<Address, Set<String>> p : pf.entrySet()) if (p.getValue().size() >= 2) ev.add("implementation " + p.getKey() + " shared by " + p.getValue().size() + " subtrees");
                for (Map.Entry<String, Set<String>> p : nm.entrySet()) if (p.getValue().size() >= 2) ev.add("direct name " + p.getKey() + " in " + p.getValue().size() + " subtrees");
                if (!ev.isEmpty()) { owned = s; evidence.put(s, String.join("; ", ev)); }
            }
            int strict = owned;
            boolean uniform = minLen == maxLen && tops.size() >= 3;
            if (uniform) owned = Math.max(owned, minLen - 1);
            if (owned < start) {
                abstractReport.add("abstract\t" + b.name + "\t-\t-\tno slot provably belongs to it (slots " + start + ".." + (minLen - 1)
                    + " in all " + desc.size() + " descendants, " + tops.size() + " subtrees, no shared implementation or repeated direct name)");
                continue;
            }
            for (int s = start; s <= owned; s++) {
                Map<String, Integer> names = new TreeMap<>();
                int unnamed = 0;
                for (String[] d : desc) {
                    String n = directName(prim.get(d[0]).get(s).pfn);
                    if (n == null) unnamed++; else names.merge(n, 1, Integer::sum);
                }
                String why = evidence.containsKey(s) ? evidence.get(s)
                    : s <= strict ? "contiguity: a higher slot of " + b.name + " is proven"
                    : "WEAKER: every one of the " + desc.size() + " descendants has a " + minLen + "-entry vtable (" + tops.size() + " subtrees)";
                if (names.size() > 1) {
                    abstractReport.add("abstract\t" + b.name + "\t" + s + "\tCONFLICT " + names + "\t" + why + "; not joined");
                    continue;
                }
                for (String[] d : desc) union.accept(d[0] + "#" + s, desc.get(0)[0] + "#" + s);
                abstractReport.add("abstract\t" + b.name + "\t" + s + "\t" + (names.isEmpty() ? "(no direct name)" : names.keySet().iterator().next()
                    + " (direct in " + names.values().iterator().next() + ", unnamed " + unnamed + ")") + "\t" + why);
            }
        }
    }

    // Named by tools/ghidra_scripts/names/*.csv: RoRApplyNames leaves a "RoR:" plate comment on functions it names.
    // (On a fresh import the CSV step runs after this script, so the end result is the same either way.)
    boolean csvNamed(Address a) {
        String c = getPlateComment(a);
        return c != null && c.startsWith("RoR:") && !c.startsWith("RoR: libkernl syscall") && !c.startsWith("RoR: self-named");
    }

    // Method name carried by a demangled linkonce function ("~" for destructors); null for anything else
    String directName(Address pfn) {
        Function f = getFunctionAt(pfn);
        if (f == null || !blockName(pfn).startsWith(LT)) return null;
        String n = f.getName();
        return n.startsWith("~") ? "~" : n;
    }

    // nearest proper ancestor of c that has an emitted (primary) vtable
    String vtAncestor(String c, Map<String, List<Slot>> prim) {
        List<String> ch = chain(c);
        for (int i = 1; i < ch.size(); i++) if (prim.containsKey(ch.get(i))) return ch.get(i);
        return null;
    }

    static final String[] IET = {"init", "exec", "term"};

    // If, for every member class, the adjacent slots carry the other two of init/exec/term in order, return the
    // missing one (case taken from the neighbours); otherwise null.
    String initExecTerm(String g, List<String> classes, int s, java.util.function.Function<String, String> find, Map<String, Set<String>> groupNames) {
        String result = null;
        for (String c : classes) {
            String r = null;
            for (int miss = 0; miss < 3 && r == null; miss++) {
                String ref = null;
                boolean ok = true;
                for (int k = 0; k < 3 && ok; k++) {
                    if (k == miss) continue;
                    String n = nameAt(c, s - miss + k, find, groupNames);
                    ok = n != null && n.equalsIgnoreCase(IET[k]);
                    if (ok) ref = n;
                }
                if (ok) r = Character.isUpperCase(ref.charAt(0)) ? Character.toUpperCase(IET[miss].charAt(0)) + IET[miss].substring(1) : IET[miss];
            }
            if (r == null || (result != null && !result.equals(r))) return null;
            result = r;
        }
        return result;
    }

    String nameAt(String c, int s, java.util.function.Function<String, String> find, Map<String, Set<String>> groupNames) {
        if (s < 1) return null;
        Set<String> n = groupNames.get(find.apply(c + "#" + s));
        return n != null && n.size() == 1 ? n.iterator().next() : null;
    }

    static String neighbourText(String n) {
        String l = n.toLowerCase();
        return l.equals("init") ? "Exec, Term (s+1, s+2)" : l.equals("exec") ? "Init, Term (s-1, s+1)" : "Init, Exec (s-2, s-1)";
    }

    // ancestors of a class, itself first, following the (single) primary base chain
    List<String> chain(String c) {
        List<String> out = new ArrayList<>();
        for (Cls k = byName.get(c); k != null && out.size() < 64; k = k.primaryBase == null ? null : byName.get(k.primaryBase)) out.add(k.name);
        return out;
    }

    String lca(Set<String> classes) {
        List<String> common = null;
        for (String c : classes) {
            List<String> ch = chain(c);
            if (common == null) common = new ArrayList<>(ch); else common.retainAll(ch);
        }
        return common == null || common.isEmpty() ? null : common.get(0);
    }

    // ---------- demangling / naming ----------
    // g++ v2 class-name mangling: <len><id>, or Q<n>/Q_<n>_ followed by n such parts.
    static String className(String m) {
        int[] pos = {0};
        String r = parseName(m, pos);
        return r == null || pos[0] != m.length() ? m : r;
    }

    static String parseName(String m, int[] pos) {
        if (pos[0] < m.length() && m.charAt(pos[0]) == 'Q') {
            pos[0]++;
            int n;
            if (m.charAt(pos[0]) == '_') { int e = m.indexOf('_', pos[0] + 1); n = Integer.parseInt(m.substring(pos[0] + 1, e)); pos[0] = e + 1; }
            else n = m.charAt(pos[0]++) - '0';
            List<String> parts = new ArrayList<>();
            for (int i = 0; i < n; i++) { String p = parseName(m, pos); if (p == null) return null; parts.add(p); }
            return String.join("::", parts);
        }
        int s = pos[0];
        while (pos[0] < m.length() && Character.isDigit(m.charAt(pos[0]))) pos[0]++;
        if (s == pos[0]) return null;
        int len = Integer.parseInt(m.substring(s, pos[0]));
        if (pos[0] + len > m.length()) return null;
        String id = m.substring(pos[0], pos[0] + len);
        pos[0] += len;
        return id;
    }

    boolean applyDemangled(String mangled, Address a) {
        try {
            MangledContext ctx = demangler.createMangledContext(mangled, dopts, currentProgram, a);
            DemangledObject o = demangler.demangle(ctx);
            if (o != null && o.applyTo(currentProgram, a, dopts, monitor)) return true;
        } catch (Exception e) { /* fall through to raw label */ }
        try { setName(a, null, mangled.replace(' ', '_')); } catch (Exception e) { return false; }
        return false;
    }

    Namespace ns(String qualified) throws Exception {
        Namespace cur = currentProgram.getGlobalNamespace();
        for (String part : qualified.split("::")) {
            Namespace n = getNamespace(cur, part);
            cur = n != null ? n : currentProgram.getSymbolTable().createClass(cur, part, SourceType.USER_DEFINED);
        }
        return cur;
    }

    void setName(Address a, Namespace n, String name) throws Exception {
        if (n == null) n = currentProgram.getGlobalNamespace();
        SymbolTable st = currentProgram.getSymbolTable();
        Function f = getFunctionAt(a);
        Symbol keep = f != null ? f.getSymbol() : null;
        // drop other symbols at this address that would collide with the new name (left over from earlier runs)
        for (Symbol s : st.getSymbols(a))
            if (s != keep && s.getName().equals(name) && s.getParentNamespace().equals(n)) s.delete();
        if (keep != null) {
            if (!(keep.getName().equals(name) && keep.getParentNamespace().equals(n))) keep.setNameAndNamespace(name, n, SourceType.USER_DEFINED);
            return;
        }
        st.createLabel(a, name, n, SourceType.USER_DEFINED).setPrimary();
    }

    void ensureFunction(Address a) {
        if (getFunctionAt(a) != null) return;
        if (getInstructionAt(a) == null) disassemble(a);
        Function f = createFunction(a, null);
        if (f == null) unresolved.add("create-function\t" + a);
    }

    // ---------- tiny constant propagation over one function ----------
    List<Call> calls(Function f) {
        List<Call> out = new ArrayList<>();
        Map<String, Long> regs = new HashMap<>();
        Instruction ins = getInstructionAt(f.getEntryPoint());
        while (ins != null && f.getBody().contains(ins.getAddress())) {
            String m = mnem(ins);
            if (m.equals("jal") || m.equals("jalr")) {
                Instruction ds = ins.getNext();
                if (ds != null) step(ds, regs);
                Address t = null;
                for (Reference r : ins.getReferencesFrom()) if (r.getReferenceType().isCall()) t = r.getToAddress();
                if (t != null) { Call c = new Call(); c.site = ins.getAddress(); c.target = t; c.regs = new HashMap<>(regs); out.add(c); }
                for (String r : new String[] {"v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9"}) regs.remove(r);
                ins = ds == null ? null : ds.getNext();
                continue;
            }
            step(ins, regs);
            ins = ins.getNext();
        }
        return out;
    }

    void step(Instruction ins, Map<String, Long> regs) {
        String m = mnem(ins);
        String dn = regName(ins, 0);
        if (dn == null || STORES.contains(m) || m.startsWith("b") || m.startsWith("j")) return;
        Long v = null;
        switch (m) {
            case "lui": v = (sc(ins, 1) << 16) & 0xffffffffL; break;
            case "li": v = sc(ins, 1) & 0xffffffffL; break;
            case "addiu": case "daddiu": {
                Long s = reg(ins, 1, regs); Scalar k = ins.getScalar(2);
                if (s != null && k != null) v = (s + k.getSignedValue()) & 0xffffffffL;
                break;
            }
            case "ori": {
                Long s = reg(ins, 1, regs); Scalar k = ins.getScalar(2);
                if (s != null && k != null) v = s | k.getUnsignedValue();
                break;
            }
            case "move": v = reg(ins, 1, regs); break;
            case "clear": v = 0L; break;
            default: break;
        }
        if (v == null) regs.remove(dn); else regs.put(dn, v);
    }

    String mnem(Instruction ins) { String m = ins.getMnemonicString(); return m.startsWith("_") ? m.substring(1) : m; }
    long sc(Instruction ins, int i) { Scalar s = ins.getScalar(i); return s == null ? 0 : s.getUnsignedValue(); }
    Long reg(Instruction ins, int i, Map<String, Long> regs) {
        String r = regName(ins, i);
        if (r == null) return null;
        if (r.equals("zero")) return 0L;
        return regs.get(r);
    }

    // R5900 GPRs are 128-bit; Ghidra reports operands as sub-registers (a1_lo etc.), so use the base name.
    String regName(Instruction ins, int i) {
        if (i >= ins.getNumOperands()) return null;
        for (Object o : ins.getOpObjects(i)) {
            if (o instanceof Register) {
                Register r = (Register) o;
                while (r.getBaseRegister() != r && r.getParentRegister() != null && r.getName().contains("_")) r = r.getParentRegister();
                return r.getName();
            }
        }
        return null;
    }

    // ---------- memory helpers ----------
    long u32(Address a) { try { return mem.getInt(a) & 0xffffffffL; } catch (Exception e) { return 0; } }
    boolean isExec(Address a) { MemoryBlock b = mem.getBlock(a); return b != null && b.isExecute(); }
    String blockName(Address a) { MemoryBlock b = mem.getBlock(a); return b == null ? "" : b.getName(); }

    String readCString(Address a) {
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < 256; i++) { byte c = mem.getByte(a.add(i)); if (c == 0) break; sb.append((char) (c & 0xff)); }
        } catch (Exception e) { return null; }
        return sb.toString();
    }

    boolean isMangledClassString(long addr) {
        String s;
        try { s = readCString(toAddr(addr)); } catch (Exception e) { return false; }
        if (s == null || s.isEmpty()) return false;
        return s.matches("(\\d+[A-Za-z_][A-Za-z0-9_$]*|Q\\d.*|t\\d.*|[a-zA-Z]{1,3})");
    }
}
