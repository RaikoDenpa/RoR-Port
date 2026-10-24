# Imports ELFR script modules into the Ghidra project ghidra/projects-seq (RuleOfRoseSeq), one program per module,
# at their linked addresses, annotated by tools/elfr.py --ghidra (native + sub-code names, debug labels, data refs).
# Inputs and annotation files go to the gitignored extracted/seq/.
# Usage: tools/ghidra/import_seq.ps1 [module files...]   (default: ENG global, world W001, area W001/a164)
param([string[]]$Modules)
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $Modules) {
    $seq = Join-Path $root 'extracted\rtpk\DATA\SEQ_ENG'
    $Modules = @("$seq\G000.GRB\0004", "$seq\W001\W000.WRD\0000", "$seq\W001\AREA.RPK\a164.ARA.d\0002")
}
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Java\jdk-26.0.1' }   # this machine's JDK
$out = Join-Path $root 'extracted\seq'
$proj = Join-Path $root 'ghidra\projects-seq'
New-Item -ItemType Directory -Force $out, $proj | Out-Null
foreach ($m in $Modules) {
    $name = (python -c "import sys; sys.path.insert(0, r'$root\tools'); import elfr; print(elfr.short_name(sys.argv[1]).replace('/', '_'))" $m)
    $bin = Join-Path $out "$name.seq"
    $tsv = Join-Path $out "$name.tsv"
    Copy-Item $m $bin -Force
    python (Join-Path $root 'tools\elfr.py') $m --ghidra $tsv
    & (Join-Path $root 'ghidra\support\analyzeHeadless.bat') $proj RuleOfRoseSeq -import $bin -overwrite `
        -loader BinaryLoader -loader-baseAddr 0x408130 -processor 'r5900:LE:32:default' -noanalysis `
        -scriptPath (Join-Path $root 'tools\ghidra') -postScript ImportElfr.java $tsv 2>&1 |
        ForEach-Object { "$_" } | Where-Object { $_ -match 'ImportElfr.java>|ERROR|Exception|^\s+at |Import succeeded|REPORT: Save' }
}
