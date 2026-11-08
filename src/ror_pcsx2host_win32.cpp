// ROR_GS and pcsx2-ror.dll (ror_pcsx2host.h). The DLL is delay-loaded (build.py: /DELAYLOAD), so the runner starts
// without it under ROR_GS=cpu; with ROR_GS=pcsx2 it is loaded here first, so a missing DLL is a log line instead of
// a delay-load exception at the first call. PCSX2 presents into a child window of the raylib window: created and
// sized on the main thread, used by the GS and VIF1 calls on the game thread (MTGS's ring takes one producer thread).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ror_pcsx2host.h"
#include "ror_pcsx2.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    constexpr wchar_t kChildClass[] = L"RorPcsx2Surface";

    bool g_active = false;      // ROR_GS=pcsx2 and the DLL is open
    HWND g_parent = nullptr;
    HWND g_child = nullptr;
    std::atomic<int> g_width{0}; // the child's client size: written on the main thread, read on the game thread
    std::atomic<int> g_height{0};
    bool g_gsOpen = false;      // game thread only
    int g_gsWidth = 0, g_gsHeight = 0;

    void logSink(void *, int level, const char *message)
    {
        ror::log("%s%s", level == ROR_PCSX2_LOG_ERROR ? "PCSX2 ERROR: " : level == ROR_PCSX2_LOG_WARNING ? "PCSX2 warning: " : "",
                 message);
    }

    LRESULT CALLBACK childProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_ERASEBKGND)
            return 1; // the swap chain covers the whole client area
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void clientSize(int &w, int &h)
    {
        RECT r{};
        GetClientRect(g_parent, &r);
        w = r.right - r.left;
        h = r.bottom - r.top;
    }

    // Game thread: PCSX2's GS with VIF1 + microVU1 on the child window, at the first call that needs it (the boot's
    // VU1 microcode upload on DMA channel 1 can come before the first VBlank).
    bool ensureGs()
    {
        if (g_gsOpen)
            return true;
        if (!g_active || !g_child)
            return false;
        const int w = g_width.load(), h = g_height.load();
        const int rc = ror_pcsx2_gs_open(g_child, w, h, ROR_PCSX2_OPEN_VIF1);
        if (rc != ROR_PCSX2_OK)
        {
            ror::log("FATAL: PCSX2's GS did not open (%d); ROR_GS=cpu runs without it", rc);
            std::exit(1);
        }
        g_gsOpen = true;
        g_gsWidth = w;
        g_gsHeight = h;
        return true;
    }
}

namespace ror
{
    bool pcsx2Open()
    {
        const char *gs = std::getenv("ROR_GS");
        if (gs && std::strcmp(gs, "cpu") == 0)
        {
            log("ROR_GS=cpu: PS2Recomp's renderer, pcsx2-ror.dll not loaded");
            return false;
        }
        if (gs && *gs && std::strcmp(gs, "pcsx2") != 0)
        {
            log("FATAL: ROR_GS=%s: expected pcsx2 or cpu", gs);
            std::exit(1);
        }
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dll(path, n);
        dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"pcsx2-ror.dll";
        if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
        {
            log("FATAL: cannot load pcsx2-ror.dll next to the runner (error %lu): build.py pcsx2, then build.py build "
                "stages it; ROR_GS=cpu runs without it", GetLastError());
            std::exit(1);
        }
        const int version = ror_pcsx2_api_version();
        if (version != ROR_PCSX2_API_VERSION || ror_pcsx2_open(ROR_PCSX2_API_VERSION, logSink, nullptr) != ROR_PCSX2_OK)
        {
            log("FATAL: pcsx2-ror.dll implements API v%d, the runner was built for v%d: rebuild both (build.py pcsx2, "
                "build.py build)", version, ROR_PCSX2_API_VERSION);
            std::exit(1);
        }
        // PCSX2's GS settings (renderer, upscale, texture filtering): ROR_GS_INI, else gs.ini in the working folder
        // (build.py run passes config/gs.ini). ROR_GS_INI=none or no file: PCSX2's defaults (gs_regress.py's 1x).
        const char *ini = std::getenv("ROR_GS_INI");
        const std::string iniPath = ini && *ini ? ini : "gs.ini";
        if (iniPath == "none")
            log("pcsx2: ROR_GS_INI=none: PCSX2's default GS settings");
        else if (FILE *f = std::fopen(iniPath.c_str(), "rb"))
        {
            std::fclose(f);
            const int rc = ror_pcsx2_gs_config(iniPath.c_str());
            if (rc != ROR_PCSX2_OK)
                log("pcsx2: %s: %s (%d)", iniPath.c_str(), rc == ROR_PCSX2_E_ARG ? "lines not understood, see above" :
                    "not applied", rc);
        }
        else if (ini && *ini)
        {
            log("FATAL: ROR_GS_INI=%s: cannot open", ini);
            std::exit(1);
        }
        g_active = true;
        return true;
    }

    void pcsx2WindowInit(void *parent)
    {
        if (!g_active)
            return;
        g_parent = static_cast<HWND>(parent);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = childProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW (narrow without UNICODE)
        wc.lpszClassName = kChildClass;
        RegisterClassExW(&wc);
        int w = 0, h = 0;
        clientSize(w, h);
        // Disabled: keyboard, mouse and gamepad focus stay with the raylib window (config/pad.ini).
        g_child = CreateWindowExW(0, kChildClass, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0,
                                  w, h, g_parent, nullptr, wc.hInstance, nullptr);
        if (!g_child)
        {
            log("FATAL: cannot create PCSX2's child window (error %lu)", GetLastError());
            std::exit(1);
        }
        g_width.store(w);
        g_height.store(h);
        log("pcsx2: child window %dx%d", w, h);
    }

    void pcsx2WindowLoop()
    {
        if (!g_child)
            return;
        int w = 0, h = 0;
        clientSize(w, h);
        if (w <= 0 || h <= 0 || (w == g_width.load() && h == g_height.load()))
            return; // minimised, or unchanged
        MoveWindow(g_child, 0, 0, w, h, TRUE);
        g_width.store(w);
        g_height.store(h);
    }

    void pcsx2WindowShutdown()
    {
        if (!g_active)
            return;
        ror_pcsx2_gs_counters c{};
        if (ror_pcsx2_gs_get_counters(&c, sizeof c) == ROR_PCSX2_OK)
            log("pcsx2: %llu vsyncs, %llu frames presented, %llu VIF1 streams (%llu stalls); VU1 output set FINISH %llu, "
                "SIGNAL %llu, LABEL %llu times", c.vsyncs, c.presents, c.vif1_streams, c.vif1_stalls, c.gs_finish,
                c.gs_signal, c.gs_label);
        ror_pcsx2_close();
        g_active = false;
        if (g_child)
            DestroyWindow(g_child);
        g_child = nullptr;
    }

    void pcsx2Vsync(const void *regs, size_t size)
    {
        if (!ensureGs())
            return;
        const int w = g_width.load(), h = g_height.load();
        if (w > 0 && h > 0 && (w != g_gsWidth || h != g_gsHeight))
        {
            ror_pcsx2_gs_resize(w, h);
            g_gsWidth = w;
            g_gsHeight = h;
        }
        ror_pcsx2_gs_vsync(regs, size);
    }

    void pcsx2Vif1(const void *data, size_t size)
    {
        if (!ensureGs())
            return;
        const int rc = ror_pcsx2_vif1_transfer(data, size);
        if (rc != ROR_PCSX2_OK)
            log("pcsx2: VIF1 stream of %zu bytes failed (%d)", size, rc);
    }

    void pcsx2Path3(const void *data, size_t size)
    {
        if (!ensureGs())
            return;
        const int rc = ror_pcsx2_gs_transfer(3, data, size);
        if (rc != ROR_PCSX2_OK)
            log("pcsx2: PATH3 packet of %zu bytes failed (%d)", size, rc);
    }

    bool pcsx2GsState(std::vector<unsigned char> &out)
    {
        size_t need = 0;
        if (!ensureGs() || ror_pcsx2_gs_save_state(nullptr, 0, &need) != ROR_PCSX2_E_BUFFER)
            return false;
        out.resize(need);
        return ror_pcsx2_gs_save_state(out.data(), out.size(), &need) == ROR_PCSX2_OK;
    }

    bool pcsx2Vu1State(std::vector<unsigned char> &out)
    {
        out.resize(ROR_PCSX2_VU1_STATE_SIZE);
        return ensureGs() && ror_pcsx2_vu1_get_state(out.data(), out.size()) == ROR_PCSX2_OK;
    }

    bool pcsx2ReadFifo(void *buf, size_t size)
    {
        return ensureGs() && ror_pcsx2_gs_read_fifo(buf, size) == ROR_PCSX2_OK;
    }

    bool pcsx2Screenshot(const char *path)
    {
        return g_gsOpen && ror_pcsx2_gs_screenshot(path) == ROR_PCSX2_OK;
    }
}
