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

    // GS diagnostics (ror_gs.cpp): tracing raster backend, host time spent in it, per-frame trace window.
    void gsInstall(PS2Runtime &runtime);
    uint64_t gsBackendNanos();
    void gsFrameMark(PS2Runtime *runtime, uint64_t frame, bool leaving);
    // The 4 MB of GS local memory, after the backend's pending work.
    void gsSnapshotVram(std::vector<uint8_t> &out);

    // PCSX2 GS dumps (ror_gsdump.cpp): ROR_GSDUMP=F[:N],... From the frame mark (end of wait_vsync_swap), each VBlank
    // and each GS->host read.
    void gsDumpInstall(PS2Runtime &runtime);
    void gsDumpFrameMark(uint64_t frame);
    void gsDumpVsync();
    void gsDumpReadFifo(uint32_t bytes);

    // Host sampling profiler (ror_profile_win32.cpp).
    void profileStart(double seconds, const char *outPath);
}
