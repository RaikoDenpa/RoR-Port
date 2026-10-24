# Setting up a fresh machine

Windows. Everything below the repo root that comes from the disc or from builds is gitignored (`disc/`, `ghidra/`,
`extracted/`, `tools/bin/`, `build/`); only code, notes and tools are in git. Never add game data (CLAUDE.md).

## 1. Clone

```
git clone --recurse-submodules https://github.com/<you>/buhbuh.git ruleofrosedecomp
cd ruleofrosedecomp
```

An existing clone without the submodule: `git submodule update --init`. `tools/PS2Recomp` is pinned to a public
upstream commit (https://github.com/ran-j/PS2Recomp); our changes to it are the patches in `tools/ps2recomp-patches/`.

## 2. Tools

| Tool | Version used | Notes |
|---|---|---|
| Git | 2.53 | with Git Bash |
| Python 3 | 3.14 (`py` launcher) | standard library only |
| JDK | 26.0.1 at `C:\Program Files\Java\jdk-26.0.1` | Ghidra 12 needs JDK 21+. Another location: set `JAVA_HOME` (the scripts use it when set) |
| Ghidra | 12.1.4 | unzip **into the repo** as `ghidra/` (so `ghidra/support/analyzeHeadless.bat` exists); projects live in `ghidra/projects/` |
| ghidra-emotionengine-reloaded | v2.1.38 (build for Ghidra 12.1.4) | Emotion Engine processor (`r5900:LE:32:default`); install via Ghidra's File → Install Extensions or unzip into `ghidra/Ghidra/Extensions/` |
| Visual Studio | 18 2026, "Desktop development with C++" | VS 2022 also has C++20: `set ROR_CMAKE_GENERATOR=Visual Studio 17 2022` before `build.py` |
| CMake | 4.3 (3.20+) | on PATH |
| ffmpeg (portable) | any recent build | optional, for listening to/viewing converted audio and movies; unzip to `tools/bin/ffmpeg/` (`tools/bin/ffmpeg/bin/ffmpeg.exe`) |
| GitHub CLI | | only to create or push the repo |

## 3. Your disc dump

Put your own dump of Rule of Rose (PAL, SLES-54218) at `disc/Rule of Rose.iso`, then:

```
py tools\iso9660.py "disc\Rule of Rose.iso" --extract extracted\disc
copy extracted\disc\SLES_542.18 disc\
xcopy /e /i extracted\disc\MODULES disc\MODULES
```

`disc/SLES_542.18` is what Ghidra and the recompiler read; `extracted/disc/` is the disc root the port reads its
files from at run time.

## 4. Ghidra project

```
powershell -ExecutionPolicy Bypass -File tools\ghidra_scripts\run_all.ps1 -Fresh
```

Imports `disc\SLES_542.18` with the EE processor, runs auto-analysis, then every naming script (classes, syscalls,
SDK strings, jump tables, function pointers, the CSV names, source files). Takes a while; reports go to a temp folder.
Optional, for the script VM notes: `tools\ghidra\import_seq.ps1` (script modules).

## 5. Build and run the port

```
py tools\port\build.py setup     # worktree of tools/PS2Recomp in extracted/ps2recomp/tree + our patches
py tools\port\build.py export    # Ghidra project -> extracted/ps2recomp/ghidra (functions, switch tables)
py tools\port\build.py config    # PS2Recomp config, function list, override bindings (with checks)
py tools\port\build.py recomp    # builds ps2_recomp, recompiles the ELF to C++, checks the output
py tools\port\build.py build     # CMake + MSVC: ps2EntryRunner.exe (first build ~17 min)
py tools\port\build.py run --frames 300 --seconds 120
```

After `export`, `py tools\port\build.py all` runs setup, config, recomp and build. The first configure downloads
PS2Recomp's dependencies (raylib, toml11, fmt, ELFIO, ...) into `tools/PS2Recomp/out/build/_deps` (ignored by the
submodule); later builds run offline. Which step to rerun after a change, run options (`--pad-script`, `--env`) and the
logs: CLAUDE.md "Build and run". Pad mapping: `config/pad.ini`.
