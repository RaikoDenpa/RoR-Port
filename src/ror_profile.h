// Host sampling profiler for the game thread (ror_profile_win32.cpp, Windows only, built without the runtime's
// precompiled header). tools/port/profile.py resolves the samples with the linker map (patch 0005: /MAP).
#pragma once

#include <cstdint>

namespace ror
{
    void log(const char *fmt, ...);
    uint64_t frame();

    // Samples the calling thread for `seconds` (about 1 kHz, stacks unwound) and writes `outPath`.
    void profileStart(double seconds, const char *outPath);
}
