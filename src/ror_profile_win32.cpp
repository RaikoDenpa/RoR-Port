// Sampling profiler: a host thread suspends the game thread about 1,000 times a second, unwinds its stack with the
// x64 unwind data (RtlLookupFunctionEntry / RtlVirtualUnwind, no PDB needed) and stores the return addresses in a
// buffer allocated up front, so nothing allocates while the game thread is stopped (it may hold the heap lock).
// The file lists each sample's frames as offsets into the exe ("e<rva>") or as the name of another module;
// tools/port/profile.py maps them to functions and object files with the linker map.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ror_profile.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace
{
    constexpr int kMaxFrames = 48;

    struct Sample
    {
        uint64_t frame;
        int depth;
        DWORD64 pc[kMaxFrames];
    };

    // Reading the suspended thread's stack can fault on a corrupt frame; SEH needs a function without C++ objects.
    bool readStack(DWORD64 address, DWORD64 &value)
    {
        __try
        {
            value = *reinterpret_cast<const DWORD64 *>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int unwind(CONTEXT c, DWORD64 *pcs)
    {
        int n = 0;
        pcs[n++] = c.Rip;
        while (n < kMaxFrames)
        {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (fn)
            {
                void *handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
            }
            else if (!readStack(c.Rsp, c.Rip)) // leaf function: the return address is at [rsp]
            {
                break;
            }
            else
            {
                c.Rsp += 8;
            }
            if (c.Rip == 0)
                break;
            pcs[n++] = c.Rip;
        }
        return n;
    }

    std::string describe(DWORD64 pc, DWORD64 exeBase, DWORD64 exeEnd)
    {
        char buf[MAX_PATH + 32];
        if (pc >= exeBase && pc < exeEnd)
        {
            std::snprintf(buf, sizeof buf, "e%llx", static_cast<unsigned long long>(pc - exeBase));
            return buf;
        }
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(pc), &m) &&
            GetModuleFileNameA(m, buf, MAX_PATH))
        {
            const char *slash = std::strrchr(buf, '\\');
            return slash ? slash + 1 : buf;
        }
        return "?";
    }
}

namespace ror
{
    void profileStart(double seconds, const char *outPath)
    {
        HANDLE game = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
        if (!game)
        {
            log("profile: cannot open the game thread (%lu)", GetLastError());
            return;
        }
        const std::string out = outPath;
        const uint64_t firstFrame = frame();
        log("profile: sampling the game thread for %.1f s from frame %llu -> %s", seconds,
            static_cast<unsigned long long>(firstFrame), out.c_str());
        std::thread([game, seconds, out, firstFrame]
                    {
            const size_t capacity = static_cast<size_t>(seconds * 1200.0) + 64;
            std::vector<Sample> samples(capacity);
            size_t count = 0;
            timeBeginPeriod(1);
            const auto t0 = std::chrono::steady_clock::now();
            const auto end = t0 + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < end && count < capacity)
            {
                if (SuspendThread(game) == static_cast<DWORD>(-1))
                    break;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                Sample &s = samples[count];
                s.depth = GetThreadContext(game, &ctx) ? unwind(ctx, s.pc) : 0;
                ResumeThread(game);
                s.frame = frame();
                if (s.depth > 0)
                    ++count;
                Sleep(1);
            }
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const uint64_t lastFrame = frame();
            timeEndPeriod(1);
            CloseHandle(game);

            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            const auto base = reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
            const DWORD64 limit = base + nt->OptionalHeader.SizeOfImage;
            FILE *f = std::fopen(out.c_str(), "w");
            if (!f)
            {
                log("profile: cannot write %s", out.c_str());
                return;
            }
            std::fprintf(f, "# exe\t%s\n# frames\t%llu\t%llu\n# seconds\t%.3f\n# samples\t%zu\n", exePath,
                         static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame), wall, count);
            for (size_t i = 0; i < count; ++i)
            {
                std::fprintf(f, "%llu\t", static_cast<unsigned long long>(samples[i].frame));
                for (int k = 0; k < samples[i].depth; ++k)
                    std::fprintf(f, k ? " %s" : "%s", describe(samples[i].pc[k], base, limit).c_str());
                std::fprintf(f, "\n");
            }
            std::fclose(f);
            log("profile: %zu samples over %.1f s (frames %llu-%llu) written", count, wall,
                static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame)); })
            .detach();
    }
}
