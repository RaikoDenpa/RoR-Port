// Exports what tools/ps2recomp/gen_config.py needs from the Ghidra project:
//   functions.tsv  entry, end (exclusive), size, qualified name (Namespace::name), one row per function
//   switches.tsv   jr address, function entry, comma-separated case targets Ghidra recovered (COMPUTED_JUMP
//                  references) for every `jr reg` other than `jr ra`; empty targets = Ghidra has none (our
//                  data/jumptables.tsv overrides carry no references, the generator takes those from the TSV)
//
// Args: <out dir>   (both files are overwritten)
//@category RuleOfRose

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;

public class RoRExportRecomp extends GhidraScript {

    @Override
    public void run() throws Exception {
        File out = new File(getScriptArgs()[0]);
        out.mkdirs();
        int nf = 0, ns = 0, nt = 0;
        try (PrintWriter fw = writer(new File(out, "functions.tsv")); PrintWriter sw = writer(new File(out, "switches.tsv"))) {
            fw.println("# entry\tend\tsize\tname");
            sw.println("# jr\tfunction\ttargets");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                AddressSetView body = f.getBody();
                if (body == null || body.getNumAddresses() == 0) continue;
                fw.printf("0x%08x\t0x%08x\t%d\t%s%n", f.getEntryPoint().getOffset(), body.getMaxAddress().getOffset() + 1,
                        body.getNumAddresses(), f.getName(true));
                nf++;
                for (Instruction in : currentProgram.getListing().getInstructions(body, true)) {
                    String m = in.getMnemonicString().replace("_", "");
                    if (!m.equals("jr") || in.getNumOperands() < 1 || "ra".equals(in.getDefaultOperandRepresentation(0))) continue;
                    TreeSet<Long> targets = new TreeSet<>();
                    for (Reference r : in.getReferencesFrom())
                        if (r.getReferenceType().isComputed() && r.getReferenceType().isJump()) targets.add(r.getToAddress().getOffset());
                    StringBuilder t = new StringBuilder();
                    for (long a : targets) t.append(t.length() == 0 ? "" : ",").append(String.format("0x%08x", a));
                    sw.printf("0x%08x\t0x%08x\t%s%n", in.getAddress().getOffset(), f.getEntryPoint().getOffset(), t);
                    ns++;
                    if (!targets.isEmpty()) nt++;
                }
            }
        }
        println(String.format("functions %d, jr sites %d (%d with recovered targets) -> %s", nf, ns, nt, out));
    }

    private static PrintWriter writer(File f) throws IOException {
        return new PrintWriter(new OutputStreamWriter(new FileOutputStream(f), StandardCharsets.UTF_8));
    }
}
