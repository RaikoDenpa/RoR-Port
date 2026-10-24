// Phase 2 step 3b: name Sony library functions from their own diagnostic strings.
//
// Sony libraries print messages prefixed with the reporting function's name, e.g.
// "sceGsSyncPath: DMA Ch.1 does not terminate\r\n". A function is renamed to <id> when
//   - it references at least one string matching ^(_?sce[A-Z]\w*)(\(\))?\s*:  and all such strings name the same <id>,
//   - no other function references a string with that same <id> prefix, and
//   - the function still has a default (FUN_) name.
// "lib<name>:" prefixes only attribute the function to a library (reported, not renamed).
//
// Args: [outFile]  optional TSV report
//@category RuleOfRose

import java.io.*;
import java.util.*;
import java.util.regex.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

public class RoRSdkStrings extends GhidraScript {

    static final Pattern SELF = Pattern.compile("^(_?sce[A-Z][A-Za-z0-9_]*)(\\(\\))?\\s*:");
    static final Pattern LIB = Pattern.compile("^(lib[a-z0-9]+)\\s*:");

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text");
        Map<Function, Set<String>> selfIds = new LinkedHashMap<>();
        Map<Function, Set<String>> libs = new LinkedHashMap<>();
        Map<Function, String> sample = new HashMap<>();
        Map<String, Set<Function>> idUsers = new HashMap<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(new AddressSet(text.getStart(), text.getEnd()), true)) {
            for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                for (Reference r : i.getReferencesFrom()) {
                    Address t = r.getToAddress();
                    MemoryBlock b = mem.getBlock(t);
                    if (b == null || b.isExecute() || (t.getOffset() & 0xffff) == 0) continue;
                    String s = cstr(mem, t);
                    if (s == null) continue;
                    Matcher m = SELF.matcher(s);
                    if (m.find()) {
                        selfIds.computeIfAbsent(f, k -> new TreeSet<>()).add(m.group(1));
                        idUsers.computeIfAbsent(m.group(1), k -> new HashSet<>()).add(f);
                        sample.putIfAbsent(f, s.trim());
                    }
                    Matcher l = LIB.matcher(s);
                    if (l.find()) { libs.computeIfAbsent(f, k -> new TreeSet<>()).add(l.group(1)); sample.putIfAbsent(f, s.trim()); }
                }
            }
        }
        PrintWriter out = getScriptArgs().length > 0 ? new PrintWriter(new File(getScriptArgs()[0]), "UTF-8") : null;
        if (out != null) out.println("address\tbefore\tself_ids\tlibs\taction\tsample");
        int named = 0;
        Set<Function> all = new LinkedHashSet<>(selfIds.keySet());
        all.addAll(libs.keySet());
        for (Function f : all) {
            Set<String> ids = selfIds.getOrDefault(f, Collections.emptySet());
            String before = f.getName();
            String action;
            if (ids.size() == 1 && idUsers.get(ids.iterator().next()).size() == 1) {
                String id = ids.iterator().next();
                if (f.getSymbol().getSource() == SourceType.DEFAULT || before.equals(id)) {
                    f.getSymbol().setNameAndNamespace(id, currentProgram.getGlobalNamespace(), SourceType.USER_DEFINED);
                    setPlateComment(f.getEntryPoint(), "RoR: self-named by its diagnostic string \"" + esc(sample.get(f)) + "\"");
                    action = "named";
                    named++;
                } else action = "kept-existing-name";
            } else if (ids.size() > 1) action = "several-ids";
            else if (ids.size() == 1) action = "id-shared-by-" + idUsers.get(ids.iterator().next()).size() + "-functions";
            else action = "library-only";
            if (out != null) out.println(f.getEntryPoint() + "\t" + before + "\t" + String.join(",", ids) + "\t" + String.join(",", libs.getOrDefault(f, Collections.emptySet()))
                + "\t" + action + "\t" + esc(sample.get(f)));
        }
        if (out != null) out.close();
        println("functions with sce/lib strings=" + all.size() + " named=" + named);
    }

    static String esc(String s) { return s == null ? "" : s.replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t"); }

    String cstr(Memory mem, Address a) {
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < 200; i++) {
                int c = mem.getByte(a.add(i)) & 0xff;
                if (c == 0) break;
                if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c >= 0x7f) return null;
                sb.append((char) c);
            }
        } catch (Exception e) { return null; }
        return sb.length() < 4 ? null : sb.toString();
    }
}
