// Installs jump-table overrides from data/jumptables.tsv (function, table address, entry count): disassembles every
// target, puts an override on the function's computed `jr`, and grows the function body over the cases.
//
// Args: [tsv]  defaults to <this script's dir>/data/jumptables.tsv
//@category RuleOfRose

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.JumpTable;

public class RoRJumpTables extends GhidraScript {

    @Override
    public void run() throws Exception {
        File f = getScriptArgs().length > 0 ? new File(getScriptArgs()[0])
                : new File(new File(getSourceFile().getParentFile().getFile(false), "data"), "jumptables.tsv");
        int ok = 0, fail = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] c = line.split("\t");
                Function fn = getFunctionAt(toAddr(c[0]));
                if (fn == null) { printerr("no function at " + c[0]); fail++; continue; }
                Address table = toAddr(c[1]);
                int n = Integer.decode(c[2]);
                ArrayList<Address> targets = new ArrayList<>();
                for (int i = 0; i < n; i++) {
                    Address a = toAddr(getInt(table.add(4L * i)) & 0xffffffffL);
                    targets.add(a);
                    if (getInstructionAt(a) == null) new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                }
                Instruction jr = null;
                for (Instruction ins : currentProgram.getListing().getInstructions(fn.getBody(), true)) {
                    if (ins.getMnemonicString().replace("_", "").equals("jr") && !ins.getDefaultOperandRepresentation(0).equals("ra")) {
                        jr = ins;
                        break;
                    }
                }
                if (jr == null) { printerr("no computed jr in " + fn.getName()); fail++; continue; }
                new JumpTable(jr.getAddress(), targets, true, 0).writeOverride(fn);
                CreateFunctionCmd.fixupFunctionBody(currentProgram, fn, monitor);
                ok++;
            }
        }
        println("jump tables applied=" + ok + " failed=" + fail);
    }
}
