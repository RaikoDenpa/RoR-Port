// Rule of Rose PC port: helpers shared by the game overrides (guest memory, arguments, calls into recompiled code,
// host log). Every override has PS2Recomp's function signature, is listed in tools/port/bindings.tsv and
// returns through ror::ret().
#pragma once

#include "ps2_runtime.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace ror
{
    using Handler = void (*)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // Integer arguments 0..7 are in a0-a3, t0-t3 (r4-r11).
    inline uint32_t arg(const R5900Context *ctx, int i) { return getRegU32(ctx, 4 + i); }
    inline int32_t sarg(const R5900Context *ctx, int i) { return static_cast<int32_t>(getRegU32(ctx, 4 + i)); }

    // v0 = value, continue at ra.
    inline void ret(R5900Context *ctx, uint32_t value)
    {
        setReturnU32(ctx, value);
        ctx->pc = getRegU32(ctx, 31);
    }

    // Host pointer to [addr, addr + size) of guest memory, or nullptr.
    uint8_t *ptr(uint8_t *rdram, uint32_t addr, uint32_t size = 1);
    std::string str(uint8_t *rdram, uint32_t addr, size_t max = 1024);
    // A game path (data/..., movie/..., cdrom0:\...;1) -> the host file under the disc root, matched case-insensitively
    // as the file seam does (ror_file.cpp); empty when the disc has no such file.
    std::string discFile(const std::string &guestPath);
    uint32_t rd32(uint8_t *rdram, uint32_t addr);
    void wr32(uint8_t *rdram, uint32_t addr, uint32_t value);
    void wr16(uint8_t *rdram, uint32_t addr, uint16_t value);
    void wr8(uint8_t *rdram, uint32_t addr, uint8_t value);

    // Runs recompiled guest code at fn with a0.. = args and returns its v0. ra and pc are restored, so the calling
    // override can return normally. Only for code that does not block in the kernel.
    uint32_t call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t fn, std::initializer_list<uint32_t> args);

    // Host log: one line per call, prefixed with the frame number; to ROR_LOG (default ror.log) and stderr.
    void log(const char *fmt, ...);
    // A guest printf: format string at fmtAddr, variable arguments from argument `first` on (registers, then stack).
    std::string format(uint8_t *rdram, R5900Context *ctx, uint32_t fmtAddr, int first);
    // Breaks into an attached debugger; otherwise stops the runtime, so `why` ends the log.
    void halt(PS2Runtime *runtime, const char *why);

    // Frames completed (MagSystem::wait_vsync_swap calls).
    uint64_t frame();

    // Sound (ror_sound.cpp): the host driver in place of rxsnd; off with ROR_SOUND=0. soundVblank mixes one VBlank
    // of the game's clock (from ror_vsync.cpp's VBlank).
    bool soundOn();
    void soundVblank();

    // Port options (ror_options.cpp, port.ini): applied each VBlank. fastForwardHeld (ror_pad.cpp): pad.ini's
    // fast_forward input is down, ror_vsync.cpp then runs 5 ms VBlanks.
    void optionsVblank(uint8_t *rdram);
    bool fastForwardHeld();
    std::string option(const char *name, const char *fallback = "");
    int optionInt(const char *name, int fallback);
    // The window (ror_window.cpp; main thread, from the external presentation's init and loop): size, fullscreen
    // (F11), the master volume.
    void windowSetup();
    void windowLoop();

    // GS (ror_gs.cpp): tracing raster backend around PS2Recomp's CPU GS, or around a null one when PCSX2's GS renders
    // (`pcsx2`, from pcsx2Open: then the raylib window stops drawing and PCSX2 presents into a child window); host
    // time spent in it, per-frame trace window.
    void gsInstall(PS2Runtime &runtime, bool pcsx2);
    uint64_t gsBackendNanos();
    void gsFrameMark(PS2Runtime *runtime, uint64_t frame, bool leaving);
    // Every VBlank of the host clock (game thread): PCSX2's GS gets the privileged registers and a VSync.
    void gsVsync(PS2Runtime *runtime);
    // The 4 MB of GS local memory, after the backend's pending work.
    void gsSnapshotVram(std::vector<uint8_t> &out);

    // PCSX2 GS dumps (ror_gsdump.cpp): ROR_GSDUMP=F[:N],... From the frame mark (end of wait_vsync_swap), each VBlank
    // and each GS->host read. gsPrivRegs: the GS privileged registers as the 8 KB at 0x12000000 (CSR.FIELD = field).
    void gsPrivRegs(PS2Runtime &runtime, bool field, std::vector<uint8_t> &out);
    // gsDumpInstall: reads ROR_GSDUMP*, true when dumps are planned (`pcsx2`: state from PCSX2, VIF1 records always);
    // the observers ror_gs.cpp installs pass the GIF packets, register writes (with their A+D packet) and VIF1 streams.
    bool gsDumpInstall(PS2Runtime &runtime, bool pcsx2);
    bool gsDumpVif1Wanted();
    void gsDumpGif(GifPathId id, const uint8_t *data, uint32_t size);
    void gsDumpRegister(uint8_t reg, uint64_t value, const uint64_t (&packet)[4]);
    void gsDumpVif1(const uint8_t *data, uint32_t size);
    void gsDumpFrameMark(uint64_t frame);
    void gsDumpVsync();
    void gsDumpReadFifo(uint32_t bytes);

    // ROR_VIF1_DUMP=F[:N] (ror_vif1dump.cpp): the frame's i3r VIF1 chain, from the end of wait_vsync_swap.
    void vif1DumpFrame(uint8_t *rdram, PS2Runtime *runtime, uint32_t magSystem, uint64_t frame);

    // Host sampling profiler (ror_profile_win32.cpp).
    void profileStart(double seconds, const char *outPath);
}
