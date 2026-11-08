// The PCSX2 side of the port: pcsx2-ror.dll behind its C API (pcsx2-ror/include/ror_pcsx2.h, notes/renderer.md 6.5a),
// selected with ROR_GS (ror_pcsx2host_win32.cpp, built without the runtime's precompiled header: windows.h and raylib
// do not mix, so the window handle and the registers cross this header as plain types).
#pragma once

#include <cstddef>
#include <vector>

namespace ror
{
    void log(const char *fmt, ...);

    // ROR_GS=pcsx2 (the default): load pcsx2-ror.dll (delay-loaded, next to the exe), check its API version and open
    // it with ror::log as its log sink (PCSX2's console included); a missing or stale DLL stops the runner with a log
    // line. Returns true: PCSX2's GS renders. ROR_GS=cpu: the DLL is never touched, PS2Recomp's renderer runs alone,
    // returns false.
    bool pcsx2Open();

    // Main (window) thread, through PS2Runtime::setExternalPresentation (patch 0014). Init: the child window PCSX2
    // presents into, over the client area of `parent` (the raylib window's HWND). Loop: keeps it at the parent's size.
    // Shutdown (the game thread has stopped): closes the DLL (and the GS with it), then the child window.
    void pcsx2WindowInit(void *parent);
    void pcsx2WindowLoop();
    void pcsx2WindowShutdown();

    // Game thread only (MTGS's ring has one producer). The first of these calls opens PCSX2's GS on the child window,
    // with its VIF1 + microVU1 (renderer plan step 3).
    // Every VBlank: a size change, then the GS privileged registers (`regs`, the 8 KB at 0x12000000) and a VSync, which
    // presents a frame.
    void pcsx2Vsync(const void *regs, size_t size);
    // Each DMA channel 1 stream (VIFcodes + data): PCSX2's VIF1, microVU1 on the MTVU thread, PATH1/PATH2 to its GS.
    void pcsx2Vif1(const void *data, size_t size);
    // A PATH3 GIF packet (GIF DMA, sceGsPutDrawEnv, A+D register writes of runtime handlers).
    void pcsx2Path3(const void *data, size_t size);
    // For GS dumps: PCSX2's GS state (GSState::Freeze) and VU1 + VIF1 state (ROR_PCSX2_VU1_STATE_SIZE bytes, the dump's
    // record 0x40); false when PCSX2 failed.
    bool pcsx2GsState(std::vector<unsigned char> &out);
    bool pcsx2Vu1State(std::vector<unsigned char> &out);
    // A GS->host transfer (TRXDIR = 1 sent on PATH3 before): `size` bytes, a multiple of 16, of PCSX2's local memory
    // after everything sent so far is drawn (renderer plan step 4); false when PCSX2 failed.
    bool pcsx2ReadFifo(void *buf, size_t size);

    // Saves the next frame PCSX2 presents as a PNG at `path`; false when the GS is not open.
    bool pcsx2Screenshot(const char *path);
}
