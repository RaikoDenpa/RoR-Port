// Applies hand-identified names from tools/ghidra_scripts/names/*.csv to SLES_542.18.
//
// CSV columns: address,namespace,name,kind,evidence
//   namespace  "" for global, or Class / Lib::Sub (created as needed)
//   kind       function (created if missing) | label
//   evidence   free text (may contain commas); stored as a "RoR:" plate comment (functions) or EOL comment (labels)
// Lines starting with # are ignored. Files are applied in name order, so later files win.
//
// Args: [namesDir]  defaults to <this script's dir>/names
//@category RuleOfRose

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class RoRApplyNames extends GhidraScript {

    @Override
    public void run() throws Exception {
        File dir = getScriptArgs().length > 0 ? new File(getScriptArgs()[0]) : new File(getSourceFile().getParentFile().getFile(false), "names");
        File[] files = dir.listFiles((d, n) -> n.endsWith(".csv"));
        if (files == null) { printerr("no names dir: " + dir); return; }
        Arrays.sort(files);
        int ok = 0, fail = 0;
        for (File f : files) {
            int lineNo = 0;
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
                for (String line; (line = r.readLine()) != null; ) {
                    lineNo++;
                    line = line.trim();
                    if (line.isEmpty() || line.startsWith("#") || line.startsWith("address,")) continue;
                    String[] c = line.split(",", 5);
                    if (c.length < 4) { printerr(f.getName() + ":" + lineNo + " bad line"); fail++; continue; }
                    try {
                        apply(toAddr(c[0].trim()), c[1].trim(), c[2].trim(), c[3].trim(), c.length > 4 ? c[4].trim() : "");
                        ok++;
                    } catch (Exception e) {
                        printerr(f.getName() + ":" + lineNo + " " + e.getMessage());
                        fail++;
                    }
                }
            }
            println("applied " + f.getName());
        }
        println("names applied=" + ok + " failed=" + fail);
    }

    void apply(Address a, String nsName, String name, String kind, String evidence) throws Exception {
        Namespace ns = namespace(nsName);
        String comment = evidence.isEmpty() ? null : "RoR: " + evidence;
        SymbolTable st = currentProgram.getSymbolTable();
        Symbol keep = null;
        if (kind.equals("function")) {
            Function f = getFunctionAt(a);
            if (f == null) {
                if (getInstructionAt(a) == null) disassemble(a);
                f = createFunction(a, null);
            }
            if (f == null) throw new Exception("cannot create function at " + a);
            keep = f.getSymbol();
        }
        for (Symbol s : st.getSymbols(a))
            if (s != keep && s.getName().equals(name) && s.getParentNamespace().equals(ns)) s.delete();
        if (keep != null) {
            if (!(keep.getName().equals(name) && keep.getParentNamespace().equals(ns))) keep.setNameAndNamespace(name, ns, SourceType.USER_DEFINED);
            if (comment != null) setPlateComment(a, comment);
        } else {
            st.createLabel(a, name, ns, SourceType.USER_DEFINED).setPrimary();
            if (comment != null) setEOLComment(a, comment);
        }
    }

    Namespace namespace(String q) throws Exception {
        Namespace cur = currentProgram.getGlobalNamespace();
        if (q.isEmpty()) return cur;
        for (String part : q.split("::")) {
            Namespace n = getNamespace(cur, part);
            cur = n != null ? n : currentProgram.getSymbolTable().createNameSpace(cur, part, SourceType.USER_DEFINED);
        }
        return cur;
    }
}
