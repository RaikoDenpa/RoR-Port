// Threads and vsync seam: the game runs on the main thread only (engine.md: no other thread runs game code once
// rxfile and movies are replaced), so the RX thread layer becomes no-ops and the VBlank interrupt becomes a host
// clock. VBlanks are counted at 50 Hz (PAL: init_graphics 0x117a28 calls sceGsResetGraph(0, 1, 3, 0)) and
// processed whenever the game waits; each one does what the two INTC 2 handlers did:
//   rxVblankHandler 0x24d120   counter 0x2f1f90 (read by rxWaitVblank)
//   MagSystem::vblank_handler 0x1176ec   [0x29819c]++, movie tick 0x1827e8(count), and when the count reaches the
//                              interval [0x2981a0] (2 = 25 fps, TskMovie sets 1): count = 0, signal m_v_sema
// MagSystem::wait_vsync_swap 0x118904 waits for that signal instead of WaitSema(m_v_sema), so a frame that took
// longer than the interval does not wait (as on hardware, where the semaphore was already signalled).
#include "ror.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr auto kVblank = std::chrono::microseconds(20000);
    // ROR_FAST_UNTIL=F: 5 ms VBlanks until frame F (diagnostics: unattended runs reach a scene sooner). The game steps
    // once per signalled frame and reads no host time, so a run is the same frame for frame, only faster. The same
    // while the player holds pad.ini's fast_forward input (ror_pad.cpp).
    constexpr auto kFastVblank = std::chrono::microseconds(5000);

    constexpr uint32_t kRxVblankCount = 0x2f1f90;
    constexpr uint32_t kMagVblankHandler = 0x1176ec;
    constexpr uint32_t kVsyncCount = 0x29819c;
    constexpr uint32_t kVsyncInterval = 0x2981a0;
    constexpr uint32_t kMovieTick = 0x1827e8;
    constexpr uint32_t kMovieObject = 0x7ddfc0;   // nonzero while rxmpeg runs (wait_vsync_swap, TskMovie::exec)
    constexpr uint32_t kMovieWait = 0x24cec8;
    constexpr uint32_t kSceDmaSync = 0x25d458;
    constexpr uint32_t kGsSwapEnv = 0x25b908;

    bool g_started = false;
    Clock::time_point g_next;
    uint32_t g_vblanks = 0;  // rxVblankHandler's counter
    uint32_t g_rxBase = 0;   // rxWaitVblank's reference count (0x842540)
    uint32_t g_handler = 0;  // INTC 2 handler installed through gs_set_vblank_handler
    uint32_t g_signals = 0;  // m_v_sema signals not yet taken by wait_vsync_swap
    uint64_t g_frames = 0;
    uint32_t g_interval = 0; // [0x2981a0] as last seen: logged when the game changes it (TskMovie, title states)
    // pump()'s catch-up since the last frame log: calls that ran more than one VBlank, the largest such burst, and
    // VBlanks dropped by the resynchronisation after 8.
    uint32_t g_bursts = 0, g_burstMax = 0, g_dropped = 0;

    void vblank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ror::wr32(rdram, kRxVblankCount, ++g_vblanks);
        if (const uint32_t interval = ror::rd32(rdram, kVsyncInterval); interval != g_interval)
        {
            ror::log("VBlank interval %u -> %u (frame %llu, VBlank %u)", g_interval, interval,
                     static_cast<unsigned long long>(g_frames), g_vblanks);
            g_interval = interval;
        }
        ror::gsDumpVsync();
        ror::gsVsync(runtime);
        ror::soundVblank();
        ror::optionsVblank(rdram);
        if (g_handler == kMagVblankHandler)
        {
            const uint32_t count = ror::rd32(rdram, kVsyncCount) + 1;
            ror::wr32(rdram, kVsyncCount, count);
            ror::call(rdram, ctx, runtime, kMovieTick, {count});
            if (ror::rd32(rdram, kVsyncInterval) <= count)
            {
                ror::wr32(rdram, kVsyncCount, 0);
                ++g_signals;
            }
        }
        else if (g_handler)
        {
            ror::call(rdram, ctx, runtime, g_handler, {});
        }
    }

    // Runs every VBlank that is due. After a long stall (debugger, loading) the clock resynchronises instead of
    // replaying them all.
    void pump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const uint64_t fastUntil = []
        {
            const char *env = std::getenv("ROR_FAST_UNTIL");
            return env ? std::strtoull(env, nullptr, 10) : 0ull;
        }();
        const auto period = g_frames < fastUntil || ror::fastForwardHeld() ? kFastVblank : kVblank;
        const auto now = Clock::now();
        if (!g_started)
        {
            g_started = true;
            g_next = now + period;
            return;
        }
        int n = 0;
        for (; now >= g_next; ++n)
        {
            if (n == 8)
            {
                g_dropped += static_cast<uint32_t>((now - g_next) / period);
                g_next = now + period;
                break;
            }
            vblank(rdram, ctx, runtime);
            g_next += period;
        }
        if (n > 1)
        {
            ++g_bursts;
            g_burstMax = std::max<uint32_t>(g_burstMax, static_cast<uint32_t>(n));
        }
    }

    void waitVblank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        pump(rdram, ctx, runtime);
        const uint32_t start = g_vblanks;
        while (g_vblanks == start && !runtime->isStopRequested())
        {
            std::this_thread::sleep_until(g_next);
            pump(rdram, ctx, runtime);
        }
    }

    uint64_t maxFrames()
    {
        static const uint64_t n = []
        {
            const char *env = std::getenv("ROR_MAX_FRAMES");
            return env ? std::strtoull(env, nullptr, 10) : 0ull;
        }();
        return n;
    }
}

uint64_t ror::frame() { return g_frames; }

// ---- RX threads (rxThInit 0x24bdc8 creates the idle and worker threads; rxfile was the only rxThCreate user) ----

void ror_InitThread(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 1); }
void ror_rxThInit(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }

void ror_rxThCreate(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ror::log("rxThCreate(entry %08x) refused: no RX threads in the port (ra %08x)", ror::arg(ctx, 0), getRegU32(ctx, 31));
    ror::ret(ctx, static_cast<uint32_t>(-1));
}

void ror_rxThWait(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }

// Worker callbacks come only from rxmpeg (0x182b4c); with movies a placeholder they are dropped, not guessed at.
void ror_rxThQueueCallback(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ror::log("rx worker callback %08x(%08x, %08x, %08x) dropped (ra %08x)", ror::arg(ctx, 0), ror::arg(ctx, 1),
             ror::arg(ctx, 2), ror::arg(ctx, 3), getRegU32(ctx, 31));
    ror::ret(ctx, 1);
}

void ror_rxThQueuedCount(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }
void ror_rxThAlarmCallback(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }

// ---- VBlank ----

// rxWaitVblank 0x24d320: 0 = until the next VBlank, 1 = no wait, n > 1 = until n VBlanks after the previous call,
// negative = read the counter. The return value of the waiting forms (an RCNT0 reading) is UNVERIFIED in use: 0.
void ror_rxWaitVblank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int32_t n = ror::sarg(ctx, 0);
    pump(rdram, ctx, runtime);
    if (n < 0)
    {
        ror::ret(ctx, g_vblanks);
        return;
    }
    if (n == 0)
        waitVblank(rdram, ctx, runtime);
    else
        while (g_vblanks < g_rxBase + static_cast<uint32_t>(n) && n > 1 && !runtime->isStopRequested())
            waitVblank(rdram, ctx, runtime);
    g_rxBase = g_vblanks;
    ror::ret(ctx, 0);
}

// gs_set_vblank_handler 0x25c850 returns the previous handler.
void ror_gs_set_vblank_handler(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t prev = g_handler;
    g_handler = ror::arg(ctx, 0);
    ror::log("vblank handler %08x (was %08x)", g_handler, prev);
    ror::ret(ctx, prev);
}

// MagSystem::wait_vsync_swap(this) 0x118904, step by step: movie wait, RCNT0 stamp (+0x30), wait for the
// vsync signal, sceDmaSync(this[+0x160], 1, 0) until the previous frame's chain is done, RCNT0 stamp (+0x28),
// FlushCache (nothing to flush here), gs_swap_env(this + 0x170, frame parity *this & 1).
void ror_wait_vsync_swap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Host time per frame: game work (between calls) and this function (waiting), averaged over the log interval.
    static Clock::time_point s_leave{};
    static double s_workMs = 0, s_waitMs = 0;
    static uint64_t s_gsNs = 0;
    const auto enter = Clock::now();
    if (s_leave != Clock::time_point{})
        s_workMs += std::chrono::duration<double, std::milli>(enter - s_leave).count();
    ror::gsFrameMark(runtime, g_frames, false);
    const uint32_t self = ror::arg(ctx, 0);
    if (ror::rd32(rdram, kMovieObject) != 0)
        ror::call(rdram, ctx, runtime, kMovieWait, {0});
    ror::wr32(rdram, self + 0x30, 0);
    pump(rdram, ctx, runtime);
    if (ror::rd32(rdram, kVsyncInterval) <= ror::rd32(rdram, kVsyncCount))
        waitVblank(rdram, ctx, runtime);
    while (g_signals == 0 && !runtime->isStopRequested())
        waitVblank(rdram, ctx, runtime);
    g_signals = 0;
    const uint32_t channel = ror::rd32(rdram, self + 0x160);
    for (int i = 0; ror::call(rdram, ctx, runtime, kSceDmaSync, {channel, 1, 0}) != 0; ++i)
    {
        if (i == 50 || runtime->isStopRequested())
        {
            ror::log("wait_vsync_swap: DMA channel %08x still busy after %d VBlanks, going on", channel, i);
            break;
        }
        waitVblank(rdram, ctx, runtime);
    }
    ror::wr32(rdram, self + 0x28, 0);
    ror::call(rdram, ctx, runtime, kGsSwapEnv, {self + 0x170, ror::rd32(rdram, self) & 1});

    ++g_frames;
    s_leave = Clock::now();
    s_waitMs += std::chrono::duration<double, std::milli>(s_leave - enter).count();
    if (g_frames == 1 || g_frames % 250 == 0)
    {
        const double n = g_frames == 1 ? 1.0 : 250.0;
        const uint64_t gsNs = ror::gsBackendNanos();
        ror::log("frame %llu done (VBlank %u; per frame: game %.1f ms incl. GS raster backend %.1f ms, vsync wait %.1f ms; "
                 "catch-up: %u bursts, largest %u VBlanks, %u dropped)",
                 static_cast<unsigned long long>(g_frames), g_vblanks, s_workMs / n, (gsNs - s_gsNs) / 1e6 / n, s_waitMs / n,
                 g_bursts, g_burstMax, g_dropped);
        s_workMs = s_waitMs = 0;
        s_gsNs = gsNs;
        g_bursts = g_burstMax = g_dropped = 0;
    }
    ror::gsFrameMark(runtime, g_frames, true);
    ror::vif1DumpFrame(rdram, runtime, self, g_frames);
    static const uint64_t s_profileFrame = []
    {
        const char *env = std::getenv("ROR_PROFILE"); // "FRAME:SECONDS"
        return env ? std::strtoull(env, nullptr, 10) : 0ull;
    }();
    if (s_profileFrame && g_frames == s_profileFrame)
    {
        const char *colon = std::strchr(std::getenv("ROR_PROFILE"), ':');
        ror::profileStart(colon ? std::atof(colon + 1) : 10.0, "profile.tsv");
    }
    // ROR_GAME_SCREENSHOT=F: the game's own debug screenshot. MagSystem::packets_end 0x1187f0 calls
    // MagSystem::screenshot 0x11860c when this+0x38 is set (file name at this+0x34): a 512 x 448 BMP of the displayed
    // frame read back with sceGsExecStoreImage, the game's only GS->host read (renderer plan step 4). Written to
    // host/ror_shot_F.bmp in the run folder (ror_sceOpen).
    static const uint64_t s_gameShot = []
    {
        const char *env = std::getenv("ROR_GAME_SCREENSHOT");
        return env ? std::strtoull(env, nullptr, 10) : 0ull;
    }();
    if (s_gameShot && g_frames == s_gameShot)
    {
        const std::string name = "host0:ror_shot_" + std::to_string(g_frames) + ".bmp";
        const uint32_t p = runtime->guestMalloc(static_cast<uint32_t>(name.size() + 1), 16u);
        if (uint8_t *s = p ? ror::ptr(rdram, p, static_cast<uint32_t>(name.size() + 1)) : nullptr)
        {
            std::memcpy(s, name.c_str(), name.size() + 1);
            ror::log("game screenshot requested: %s (MagSystem %08x, file name pointer was %08x)", name.c_str(), self,
                     ror::rd32(rdram, self + 0x34));
            ror::wr32(rdram, self + 0x34, p);
            ror::wr32(rdram, self + 0x38, 1);
        }
    }
    if (maxFrames() && g_frames >= maxFrames())
    {
        ror::log("reached ROR_MAX_FRAMES = %llu, stopping", static_cast<unsigned long long>(maxFrames()));
        runtime->requestStop();
    }
    ror::ret(ctx, 0);
}
