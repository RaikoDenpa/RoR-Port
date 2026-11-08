// Shared inside pcsx2-ror.dll (not part of the C API).
#pragma once

#include "common/WindowInfo.h"

#include <optional>

namespace ror_pcsx2
{
    // To the runner's log sink (ror_pcsx2_open); level is ROR_PCSX2_LOG_*.
    void log(int level, const char* fmt, ...);

    // ror_pcsx2_open succeeded and ror_pcsx2_close has not run.
    bool isOpen();

    // gs.cpp: the window ror_pcsx2_gs_open was given (Host::AcquireRenderWindow), none before; a presented frame
    // (Host::BeginPresentFrame).
    std::optional<WindowInfo> renderWindow();
    void countPresent();

    // vif1.cpp: PCSX2's VM memory, VIF1 and microVU1 (MTVU) up before MTGS opens, down after it closes; counters.
    bool vif1Open();
    void vif1Close();
    bool vif1IsOpen();
    void vif1Counters(unsigned long long &streams, unsigned long long &stalls);
} // namespace ror_pcsx2
