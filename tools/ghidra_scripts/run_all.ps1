# Re-runs the whole naming pipeline on the Ghidra project, optionally from a fresh import.
#
#   .\tools\ghidra_scripts\run_all.ps1 [-Fresh] [-ProjectDir <dir>] [-ProjectName <name>] [-ReportDir <dir>]
#
#   -Fresh       delete and re-import disc\SLES_542.18 (full auto-analysis, EE language r5900:LE:32:default)
#   -ReportDir   where the scripts write their TSV reports (default: a temp folder)
#
# Order matters: classes first (vtable/override names), then syscall wrappers (keeps the EE extension's names),
# self-named SDK functions, jump-table overrides (data/jumptables.tsv, so switch cases join their functions),
# functions only data points at (vtable leaves, default natives, i3r handlers; after the jump tables, which it skips),
# the hand-written CSV names (override everything they list), and source-file tags last
# (the class-adjacency rule uses the class names).
param(
    [switch]$Fresh,
    [string]$ProjectDir = (Join-Path $PSScriptRoot '..\..\ghidra\projects'),
    [string]$ProjectName = 'RuleOfRose',
    [string]$ReportDir = (Join-Path ([IO.Path]::GetTempPath()) 'ror-ghidra-reports')
)
# Not 'Stop': Windows PowerShell 5.1 turns the JVM's stderr warnings into terminating errors under 2>&1.
# Failures are detected through analyzeHeadless's exit code instead.
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$headless = Join-Path $root 'ghidra\support\analyzeHeadless.bat'
$elf = Join-Path $root 'disc\SLES_542.18'
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Java\jdk-26.0.1' }
New-Item -ItemType Directory -Force $ProjectDir, $ReportDir | Out-Null
$ProjectDir = (Resolve-Path $ProjectDir).Path  # Ghidra rejects path elements starting with '.' (e.g. '..')

function Invoke-Headless([string[]]$a) {
    & $headless @a 2>&1 | ForEach-Object { "$_" } |
        Where-Object { $_ -match '\.java> |ERROR|REPORT: (Import|Save|Analysis)' } |
        ForEach-Object { $_ -replace '^INFO\s+', '' -replace ' \(GhidraScript\)\s*$', '' -replace ' \(HeadlessAnalyzer\)\s*$', '' }
    if ($LASTEXITCODE -ne 0) { throw "analyzeHeadless failed ($LASTEXITCODE)" }
}

if ($Fresh) {
    Invoke-Headless @($ProjectDir, $ProjectName, '-import', $elf, '-processor', 'r5900:LE:32:default', '-overwrite')
}
$steps = @(
    @('RoRClasses.java', (Join-Path $ReportDir 'classes')),
    @('RoRSyscalls.java', (Join-Path $ReportDir 'syscalls.tsv')),
    @('RoRSdkStrings.java', (Join-Path $ReportDir 'sdk_strings.tsv')),
    @('RoRJumpTables.java'),
    @('RoRFunctionPointers.java', (Join-Path $ReportDir 'function_pointers.tsv')),
    @('RoRApplyNames.java'),
    @('RoRSourceFiles.java', (Join-Path $ReportDir 'sources'))
)
foreach ($s in $steps) {
    $a = @($ProjectDir, $ProjectName, '-process', 'SLES_542.18', '-noanalysis', '-scriptPath', $PSScriptRoot, '-postScript') + $s
    Invoke-Headless $a
}
Write-Host "reports: $ReportDir"
Write-Host "notes:   python tools\ghidra_scripts\report_classes.py $ReportDir\classes > ...  /  report_sources.py $ReportDir\sources > ..."
