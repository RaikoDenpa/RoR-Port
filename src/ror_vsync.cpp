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

#include <chrono>
#include <cstdlib>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr auto kVblank = std::chrono::microseconds(20000);

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

    void vblank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ror::wr32(rdram, kRxVblankCount, ++g_vblanks);
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
        const auto now = Clock::now();
        if (!g_started)
        {
            g_started = true;
            g_next = now + kVblank;
            return;
        }
        for (int n = 0; now >= g_next; ++n)
        {
            if (n == 8)
            {
                g_next = now + kVblank;
                break;
            }
            vblank(rdram, ctx, runtime);
            g_next += kVblank;
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
    const auto enter = Clock::now();
    if (s_leave != Clock::time_point{})
        s_workMs += std::chrono::duration<double, std::milli>(enter - s_leave).count();
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
        ror::log("frame %llu done (VBlank %u; per frame: game %.1f ms, vsync wait %.1f ms)",
                 static_cast<unsigned long long>(g_frames), g_vblanks, s_workMs / n, s_waitMs / n);
        s_workMs = s_waitMs = 0;
    }
    if (maxFrames() && g_frames >= maxFrames())
    {
        ror::log("reached ROR_MAX_FRAMES = %llu, stopping", static_cast<unsigned long long>(maxFrames()));
        runtime->requestStop();
    }
    ror::ret(ctx, 0);
}
