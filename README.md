# Rule of Rose — PC Port

A native PC port of *Rule of Rose* (PlayStation 2, Punchline, 2006), built by reverse engineering the
PAL release (`SLES_542.18`) and statically recompiling it to C++.

> **Private, personal, non-commercial project.** This repository contains **no game data**: no disc image,
> executable, assets, or text extracted from the game. Everything here is original code, tools and research
> notes. Running the port requires your own legally dumped copy of the game.

---

## Status

| # | Milestone | Status |
|---|---|---|
| 1 | Recompiled game builds | ✅ |
| 2 | Boots to `main` | ✅ |
| 3 | Main loop runs, first frames drawn | ✅ |
| 4 | Title screen with input | 🟡 Language select, publisher logos, disclaimer and Title menu reached; colours wrong (VIF1 upload bug, fix pending); attract-mode movie stalls on a placeholder |
| 5 | First area: Jennifer on screen | ⬜ |
| 6 | Playable: walking, menus, Brown, saving | ⬜ |
| 7 | Full playthrough | ⬜ |

**Current focus:** fixing the VIF1 upload bug and the display height, getting past the title into a new
game, and integrating PCSX2's GS renderer. The current software GS is about 96% of frame time, so the port
runs at roughly half speed. See [`notes/renderer.md`](notes/renderer.md).

## How it works

The game's MIPS R5900 code is **statically recompiled** into C++ with
[PS2Recomp](https://github.com/ran-j/PS2Recomp). Only the points where the game touches PS2 hardware or
system software are replaced with native PC code. All game logic, the event-script interpreter and the
3D library run unchanged, so behaviour matches the original.

```
SLES_542.18 ──► Ghidra analysis ──► function list, names, jump tables
                                          │
                                          ▼
                     tools/port/gen_config.py ──► PS2Recomp ──► C++ (≈8,100 functions)
                                                                   │
              src/ (seam overrides) + PS2Recomp runtime + patches ─┴─► ps2EntryRunner.exe
                                                                   │
                                   your own disc dump (read at runtime)
```

### Seams: where the PS2 is replaced

| Seam | Replaced at | State |
|---|---|---|
| File I/O | `rxFile*` API + libkernl `sceOpen`/`sceRead` | ✅ host files from your disc dump |
| Threads / vsync | RX thread layer, `MagSystem::wait_vsync_swap` | ✅ single-threaded, 25 Hz host frame loop |
| Input | libpad2/libdbc raw pad data | ✅ keyboard + gamepad (`config/pad.ini`) |
| Rendering | VIF1 → GS | 🟡 PS2Recomp's software GS; PCSX2 GS planned |
| Movies | `TskMovie` / rxmpeg | ⬜ placeholder (skipped); ffmpeg planned |
| Sound | rxsnd client API (26 functions) | ⬜ placeholder (silent) |
| Memory card | libmc API | ⬜ placeholder (empty card); host files planned |

## Repository layout

```
CLAUDE.md                 project rules and workflow
notes/                    research: one file per subsystem (start with engine.md and port.md)
  formats/                file formats: RTPK archives, I3D models, TIM2, PSS, STR, XVAB, ELFR scripts
src/                      the port: game overrides at each seam (C++)
config/pad.ini            input mapping
tools/
  port/                   config generator, build/run driver, profiler and GS trace readers
  ps2recomp-patches/      numbered patch series applied to PS2Recomp
  ghidra_scripts/         reproducible naming pipeline (run_all.ps1 rebuilds every name from a fresh import)
  *.py                    asset tools: iso9660, rtpk, tim2png, i3d2obj, str2wav, xvab, pss_audio, elfr, vudis …
  PS2Recomp/              submodule, pinned commit
```

Not tracked (gitignored): `disc/`, `extracted/` (your dump, extracted assets, generated C++, builds, logs),
`ghidra/` (Ghidra projects).

## Research notes

The analysis behind the port lives in [`notes/`](notes/). Some highlights:

| File | Contents |
|---|---|
| [`engine.md`](notes/engine.md) | one frame from `main` to the GS, state machine, render path, threads |
| [`classes.md`](notes/classes.md) | 290 C++ classes rebuilt from RTTI and vtables |
| [`script-vm.md`](notes/script-vm.md) | the event-script interpreter and its 41 native calls |
| [`file-system.md`](notes/file-system.md) | loading path from request to disc read, disc coverage |
| [`port-strategy.md`](notes/port-strategy.md) | why recompilation, the seams, milestones |
| [`renderer.md`](notes/renderer.md) | frame profile, GS diagnosis, renderer options |
| [`glossary.md`](notes/glossary.md) | naming conventions (`dog/` is the 3D engine, `Bwn` is Brown) |

## Building

Full setup for a new machine is in [`SETUP.md`](SETUP.md). In short:

**Requirements:** Windows, Visual Studio 2026 (MSVC) + CMake, Python 3, Ghidra with the
[Emotion Engine processor module](https://github.com/chaoticgd/ghidra-emotionengine-reloaded),
and your own dump of the PAL disc (`SLES_542.18`).

```powershell
git clone --recurse-submodules <this repo>
# 1. Extract your disc dump into extracted/disc/  (python tools/iso9660.py)
# 2. Rebuild the Ghidra project and names
tools/ghidra_scripts/run_all.ps1 -Fresh
# 3. Generate config, recompile, build, run
python tools/port/build.py setup
python tools/port/build.py recomp
python tools/port/build.py build
python tools/port/build.py run
```

The first full build takes about 17 minutes. Rebuilding after a change to one function takes seconds.

### Useful run options

| Option | Does |
|---|---|
| `build.py run --frames N` | stop after N frames |
| `build.py run --pad-script "100+4:cross"` | press buttons at given frames (unattended runs) |
| `--env ROR_PROFILE=FRAME:SECONDS` | sampling profile → `tools/port/profile.py` |
| `--env ROR_GS_TRACE=FRAME --env ROR_GS_VRAM=1` | dump one frame's GS draws and memory → `tools/port/gs_trace.py`, `gs_vram.py` |

Logs go to `extracted/ps2recomp/run/` (`ror.log` includes the game's own script debug prints).

## Project rules

- **Never commit game data.** Nothing from the disc, extracted assets or game-derived text goes into git.
- Every finding cites evidence (an address, a function, a tool result or a runtime observation). Guesses are
  marked `UNVERIFIED`.
- Never hand-edit generated C++. Fixes go into the config generator, `src/` overrides, or the patch series.

## Licence

This project builds on [PS2Recomp](https://github.com/ran-j/PS2Recomp) (GPL-3.0), and the planned renderer
uses [PCSX2](https://github.com/PCSX2/pcsx2) (GPL-3.0), so the port as a whole falls under GPL-3.0.
*Rule of Rose* is © Sony Computer Entertainment / Punchline. This project is not affiliated with or endorsed
by them, and it does not include or distribute any of their content.
