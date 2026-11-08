# Setting up a fresh machine

Windows. Everything below the repo root that comes from the disc or from builds is gitignored (`disc/`, `ghidra/`,
`extracted/`, `tools/bin/`, `build/`); only our code and tools are in git. Never add game data.

## 1. Clone

```
git clone --recurse-submodules https://github.com/RaikoDenpa/RoR-Port.git ruleofrosedecomp
cd ruleofrosedecomp
```

An existing clone without the submodules: `git submodule update --init`. Both are pinned to public upstream commits:
`tools/PS2Recomp` (https://github.com/ran-j/PS2Recomp) and `tools/pcsx2` (https://github.com/PCSX2/pcsx2, v2.8.2,
shallow). Our changes to them are the patch series in `tools/ps2recomp-patches/` and `tools/pcsx2-patches/`, applied
by `build.py setup` onto worktrees in `extracted/`; the submodules themselves stay clean.

## 2. Tools

| Tool | Version used | Notes |
|---|---|---|
| Git | 2.53 | with Git Bash |
| Python 3 | 3.14 (`py` launcher) | standard library only |
| Visual Studio | 18 2026, "Desktop development with C++" | includes the C++ CMake tools (CMake and Ninja), which PCSX2's dependency build uses. VS 2022 also works: `set ROR_CMAKE_GENERATOR=Visual Studio 17 2022` before `build.py` |
| CMake | 4.3 (3.20+) | on PATH |
| 7-Zip's `7zr.exe` | any | the standalone console version from https://www.7-zip.org, saved as `tools\bin\7zr.exe`; `build.py ffmpeg` unpacks FFmpeg with it |
| JDK | 26.0.1 at `C:\Program Files\Java\jdk-26.0.1` | Ghidra 12 needs JDK 21+. Another location: set `JAVA_HOME` (the scripts use it when set) |
| Ghidra | 12.1.4 | unzip **into the repo** as `ghidra/` (so `ghidra/support/analyzeHeadless.bat` exists); projects live in `ghidra/projects/` |
| ghidra-emotionengine-reloaded | v2.1.38 (build for Ghidra 12.1.4) | Emotion Engine processor (`r5900:LE:32:default`); install via Ghidra's File → Install Extensions or unzip into `ghidra/Ghidra/Extensions/` |
| NASM | any | optional: one of PCSX2's dependencies builds faster with it |
| ffmpeg (portable) | any recent build | optional, for listening to/viewing converted audio and movies; unzip to `tools/bin/ffmpeg/` |
| GitHub CLI | | only to create or push the repo |

The build downloads what it needs at pinned versions, so the first run needs internet access: PS2Recomp's
dependencies (raylib, toml11, fmt, ELFIO, …, into `tools/PS2Recomp/out/build/_deps`, ignored by the submodule),
PCSX2's Windows dependencies (with the SHA-256 checks of PCSX2's own script) and FFmpeg's shared build (gyan.dev 8.1.2,
SHA-256 checked). Later builds run offline.

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
Optional, for the script modules: `tools\ghidra\import_seq.ps1`.

## 5. Build and run the port

```
py tools\port\build.py setup        # worktrees of tools/PS2Recomp and tools/pcsx2 in extracted/ + our patch series
py tools\port\build.py pcsx2-deps   # once (~20 min): PCSX2's Windows dependencies at the pinned tag (no Qt)
py tools\port\build.py ffmpeg       # once: FFmpeg's avcodec/avutil DLLs for the movie player (needs tools\bin\7zr.exe)
py tools\port\build.py export       # Ghidra project -> extracted/ps2recomp/ghidra (functions, switch tables)
py tools\port\build.py all          # setup, config, recomp, pcsx2, build: ps2EntryRunner.exe + pcsx2-ror.dll
py tools\port\build.py run --frames 0 --seconds 3600
```

The first `all` takes about 17 minutes for the recompiled game, plus PCSX2's build. Without the FFmpeg DLLs the port
still runs and skips the movies. The run folder is `extracted\ps2recomp\run\` (`ror.log`, `stdout.txt`, `stderr.txt`,
the memory card in `memcard\`).

The playtest package (the launcher, the runner and its DLLs, no game files):

```
py tools\port\build.py dist         # -> extracted\dist\RuleOfRose-playtest-<commit>.zip
```

### What to rerun after a change

| Changed | Run |
|---|---|
| `src/` only | `build` |
| override rows in `tools/port/bindings.tsv` | `config`, `build` |
| stub/syscall rows, `tools/port/gen_config.py` | `config`, `recomp`, `build` (a changed stub list rebuilds everything) |
| names or functions in Ghidra | `export`, `config`, `recomp`, `build` |
| `pcsx2-ror/` | `pcsx2`, `build` (build stages the DLL) |
| a patch in `tools/pcsx2-patches/` | `setup`, `pcsx2`, `build` |
| a patch in `tools/ps2recomp-patches/` | `setup`, `build` |
| `launcher/` | `dist` |

Run options (`--frames`, `--pad-script`, `--env ROR_...`): [README.md](README.md) "Useful run options". Pad mapping: `config/pad.ini`; PCSX2's GS
settings: `config/gs.ini`.
