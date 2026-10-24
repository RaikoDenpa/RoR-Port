# Ghidra naming scripts

Re-create every Phase 2 name in the Ghidra project (`ghidra/projects/RuleOfRose.gpr`):

```powershell
.\tools\ghidra_scripts\run_all.ps1          # re-run on the existing project
.\tools\ghidra_scripts\run_all.ps1 -Fresh   # re-import disc\SLES_542.18 first (EE language r5900:LE:32:default)
```

| Script | Step | Names |
|---|---|---|
| `RoRClasses.java` | 2.1 | demangled linkonce functions, `Class::__tf/__ti/vtable`, `__rtti_user/si`, `vfunc_N`, override names (through bases without a vtable, slot consensus for those bases, Init/Exec/Term pattern; evidence in `abstract_slots.tsv`) |
| `RoRSyscalls.java` | 2.3 | libkernl syscall wrappers from `data/ee_syscalls.tsv` |
| `RoRSdkStrings.java` | 2.3 | SDK functions that name themselves in their own messages (`"sceGsSyncPath: …"`) |
| `RoRJumpTables.java` | 5 | jump-table overrides from `data/jumptables.tsv` (script natives whose switch Ghidra does not recover) |
| `RoRFunctionPointers.java` | port | creates the ~695 functions only data points at (vtable leaves, default natives, i3r node handlers), skipping jump tables; marks each with a `RoR:` plate comment, removes its own stale ones. They get `FUN_` names (it runs after `RoRClasses.java`) |
| `RoRExportRecomp.java` | port | export for `tools/port/gen_config.py`: functions with qualified names, every `jr reg` site with its case targets |
| `RoRApplyNames.java` | all | hand-identified names in `names/*.csv` (address, namespace, name, kind, evidence) |
| `RoRSourceFiles.java` | 2.2 | function tags `src:<file>` / `src~<method>:<file>` |
| `report_classes.py`, `report_sources.py` | — | turn the TSV reports into the tables in `notes/classes.md`, `notes/source-files.md` |

Every name carries its evidence: as a `RoR:` plate comment, in the CSV's evidence column, or in the notes.
