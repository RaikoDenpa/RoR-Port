# Rule of Rose — PC Port

A native PC port of *Rule of Rose* (PlayStation 2, Punchline, 2006), built by reverse engineering the
PAL release (`SLES_542.18`) and statically recompiling it to C++.

> **A non-commercial fan project.** This repository contains **no game data**: no disc image, executable,
> assets, or text extracted from the game. Everything here is original code and tools. Running the port
> requires your own legally dumped copy of the game (PAL, SLES-54218).

---

## Status

| # | Milestone | Status |
|---|---|---|
| 1 | Recompiled game builds | ✅ |
| 2 | Boots to `main` | ✅ |
| 3 | Main loop runs, first frames drawn | ✅ |
| 4 | Title screen with input | ✅ language select, logos, attract movie, Title menu |
| 5 | First area: Jennifer on screen | ✅ |
| 6 | Playable: walking, menus, Brown, saving | ✅ menus, items, Brown's Find, saving and loading, combat |
| 7 | Full playthrough | 🟡 chapters 1 and 2 played through; chapter 3 up to the end of the rabbit chase |

What works today: the story with its in-engine events, movies with sound, music, voices and sound effects,
fighting (with enemies grabbing, getting knocked down and the game-over screen), saving to and loading from a
memory card in a host folder, and a launcher with display, graphics and sound settings.

**Current focus:** the rest of chapter 3 (its boss fight) and the later chapters, plus feedback from playtesters.

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
           pcsx2-ror.dll (PCSX2's GS, VU1, VIF1) ◄──────────────────┤
                                   your own disc dump (read at runtime)
```

### Seams: where the PS2 is replaced

| Seam | Replaced at | State |
|---|---|---|
| File I/O | `rxFile*` API (including the streaming ring) + libkernl `sceOpen`/`sceRead` | ✅ host files from your disc dump |
| Threads / vsync | RX thread layer, the VBlank interrupt, `MagSystem::wait_vsync_swap` | ✅ 50 Hz VBlank clock (PAL), the game's 25 fps |
| Input | libpad2 raw pad data | ✅ keyboard + gamepad (`config/pad.ini`) |
| Rendering | VIF1 → VU1 → GS | ✅ PCSX2's VIF1, microVU1 and GS in `pcsx2-ror.dll` (D3D11/12, Vulkan, OpenGL; upscaling); PS2Recomp's software GS as a fallback |
| Movies | rxmpeg stream and decode functions | ✅ our player: our PSS demuxer, FFmpeg's MPEG-2 decoder, our PS-ADPCM audio |
| Sound | rxsnd client API (the IOP sound driver's calls) | ✅ our driver: XVAB sound-effect banks, music and voice streams, volumes, mixed at 48 kHz |
| Memory card | libmc API | ✅ host folders (`user\memcard` in the launcher's package) |
| Window | raylib window + PCSX2's swap chain | ✅ resizable, fullscreen (F11), aspect ratio from PCSX2's settings |

## Playing

The port is distributed only as private playtest builds to people who own the game. `python tools/port/build.py
dist` makes that package: `RuleOfRose.exe` (the launcher), the runner and its DLLs, and no game files.

The launcher is a settings window: display (window or fullscreen, window size, 4:3 or stretched), graphics
(renderer, internal resolution, texture filtering), game (skip the intro, movies, volume), controls, saves and
logs. The first time you press Play it asks for your disc image, checks that it is the PAL SLES-54218 release,
and copies the game's files next to it.

In the game: **F11** toggles fullscreen, holding **F** fast-forwards (4x), **Start** skips movies. The default
keys are in `config/pad.ini` (WASD to move, X/Space for Cross, C/Backspace for Circle, Enter for Start, Q/E for
L1/R1); a gamepad works out of the box.

## Repository layout

```
src/                      the port: game overrides at each seam (C++), window and options
pcsx2-ror/                the PCSX2-side library (pcsx2-ror.dll): GS/VU1/VIF1 host layer and its C API
launcher/                 RuleOfRose.exe: the settings window, disc-image check and extraction, tester README
config/                   pad.ini (input mapping), gs.ini (PCSX2's GS settings)
tools/
  port/                   config generator, build/run/package driver, play and regression tools, diagnostics
  ps2recomp-patches/      numbered patch series applied to PS2Recomp
  pcsx2-patches/          numbered patch series applied to PCSX2
  ghidra_scripts/         reproducible naming pipeline (run_all.ps1 rebuilds every name from a fresh import)
  *.py                    asset tools: iso9660, rtpk, tim2png, i3d2obj, str2wav, xvab, pss_audio, elfr, vudis …
  PS2Recomp/, pcsx2/      submodules, pinned commits
```

Not tracked (gitignored): `disc/`, `extracted/` (your dump, extracted assets, generated C++, builds, logs,
packages), `ghidra/` (Ghidra projects).

## Building

Full setup for a new machine is in [`SETUP.md`](SETUP.md). In short:

**Requirements:** Windows, Visual Studio 2026 (MSVC) + CMake, Python 3, Ghidra with the
[Emotion Engine processor module](https://github.com/chaoticgd/ghidra-emotionengine-reloaded),
and your own dump of the PAL disc (`SLES_542.18`).

```powershell
git clone --recurse-submodules https://github.com/RaikoDenpa/RoR-Port.git
# 1. Extract your disc dump into extracted/disc/  (python tools/iso9660.py)
# 2. Rebuild the Ghidra project and names
tools/ghidra_scripts/run_all.ps1 -Fresh
# 3. Worktrees with our patches, then the dependencies (once): PCSX2's Windows dependencies and FFmpeg's
#    decoder DLLs (pinned, checksummed; FFmpeg's step needs 7-Zip's 7zr.exe in tools/bin)
python tools/port/build.py setup
python tools/port/build.py pcsx2-deps
python tools/port/build.py ffmpeg
# 4. Export from Ghidra, then setup + config + recompile + PCSX2 + build
python tools/port/build.py export
python tools/port/build.py all
# 5. Run, or package the launcher build
python tools/port/build.py run --frames 0 --seconds 3600
python tools/port/build.py dist
```

A full build takes about 17 minutes (PCSX2's part is extra on the first run). Rebuilding after a change to one
override takes seconds.

### Useful run options

| Option | Does |
|---|---|
| `build.py run --frames N` | stop after N frames (0 = never) |
| `build.py run --pad-script "100+4:cross"` | press buttons at given frames (unattended runs) |
| `--env ROR_MOVIES=0`, `--env ROR_SOUND=0` | skip movies; silence the sound driver (the regression tools use both) |
| `--env ROR_GS=cpu` | PS2Recomp's software GS instead of PCSX2's |
| `--env ROR_SCREENSHOT=F,…` | save the presented frame at those frames |
| `--env ROR_PROFILE=FRAME:SECONDS` | sampling profile → `tools/port/profile.py` |
| `--env ROR_GS_TRACE=FRAME --env ROR_GS_VRAM=1` | dump one frame's GS draws and memory → `tools/port/gs_trace.py`, `gs_vram.py` |
| `tools/port/play_regress.py`, `gs_regress.py` | gameplay and renderer regression checks against reference runs |

Logs go to `extracted/ps2recomp/run/` (`ror.log` includes the game's own script debug prints).

## Project rules

- **Never commit game data.** Nothing from the disc, extracted assets or game-derived text goes into git.
- Every finding cites evidence (an address, a function, a tool result or a runtime observation). Guesses are
  marked `UNVERIFIED`.
- Never hand-edit generated C++. Fixes go into the config generator, `src/` overrides, or the patch series.

## Licence

This project builds on [PS2Recomp](https://github.com/ran-j/PS2Recomp) (GPL-3.0) and its renderer uses
[PCSX2](https://github.com/PCSX2/pcsx2) (GPL-3.0), so the port as a whole falls under GPL-3.0. Movies are
decoded with [FFmpeg](https://ffmpeg.org) (a GPL build, loaded at runtime).
*Rule of Rose* is © Sony Computer Entertainment / Punchline. This project is not affiliated with or endorsed
by them, and it does not include or distribute any of their content.
