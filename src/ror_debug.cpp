// Debug plumbing: script labels (native 32), the two AssertFail copies, printf and the RX printf, all to the host
// log. Evidence: native_32_debug_print 0x20a5c0 (sub 10 = label string in a1, script-vm.md), AssertFail 0x21730c /
// 0x216ed0 ("Assertion Failed: %s(%d)   %s\n", then spin), printf 0x262050, rx printf 0x243e80 (formats into a
// 1 KB buffer, then printf or sceWrite to its tty fd).
#include "ror.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    // seq_cur_slot (0 global, 1 world, 2 area) and the module numbers GamMan keeps: world GamMan+0x21f50
    // (GamMan::loadWorld 0x1c34ec stores its argument there), area GamMan+0x21f7c (GamMan::loadArea 0x1c5c08
    // formats A%03d.ARA from it).
    constexpr uint32_t kSeqCurSlot = 0x7decd0;
    constexpr uint32_t kGamMan = 0x7ab1c0;

    std::string module(uint8_t *rdram)
    {
        char buf[16];
        switch (ror::rd32(rdram, kSeqCurSlot))
        {
        case 0:
            return "global";
        case 1:
            std::snprintf(buf, sizeof buf, "W%03d", static_cast<int32_t>(ror::rd32(rdram, kGamMan + 0x21f50)));
            return buf;
        case 2:
            std::snprintf(buf, sizeof buf, "A%03d", static_cast<int32_t>(ror::rd32(rdram, kGamMan + 0x21f7c)));
            return buf;
        default:
            std::snprintf(buf, sizeof buf, "slot%u", ror::rd32(rdram, kSeqCurSlot));
            return buf;
        }
    }

    // Consecutive repeats of one label (a script waiting in a loop) are folded into a count.
    std::string g_last;
    unsigned g_repeats = 0;

    std::string trimNewlines(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        return s;
    }
}

void ror_native32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    if (ror::arg(ctx, 0) == 10)
    {
        const std::string line = module(rdram) + ": " + ror::str(rdram, ror::arg(ctx, 1), 0x200);
        if (line == g_last)
        {
            ++g_repeats;
        }
        else
        {
            if (g_repeats)
                ror::log("script (previous label repeated %u more times)", g_repeats);
            ror::log("script %s", line.c_str());
            g_last = line;
            g_repeats = 0;
        }
    }
    ror::ret(ctx, 0);
}

void ror_AssertFail(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const std::string file = ror::str(rdram, ror::arg(ctx, 0));
    const std::string expr = ror::str(rdram, ror::arg(ctx, 2));
    char why[1200];
    std::snprintf(why, sizeof why, "assertion failed: %s(%d): %s (ra %08x)", file.c_str(), ror::sarg(ctx, 1), expr.c_str(),
                  getRegU32(ctx, 31));
    if (file.find("bedmemory") != std::string::npos)
    {
        // The game heap (MagSystem heap 0x7135c0, set up by 0x100354/0x1003f8): descriptor, then its first block.
        auto dump = [&](const char *what, uint32_t a)
        {
            ror::log("  %s %08x: %08x %08x %08x %08x %08x %08x %08x %08x", what, a, ror::rd32(rdram, a), ror::rd32(rdram, a + 4),
                     ror::rd32(rdram, a + 8), ror::rd32(rdram, a + 12), ror::rd32(rdram, a + 16), ror::rd32(rdram, a + 20),
                     ror::rd32(rdram, a + 24), ror::rd32(rdram, a + 28));
        };
        dump("heap", 0x7135c0);
        dump("block", ror::rd32(rdram, 0x7135c0));
    }
    ror::halt(runtime, why);
    ror::ret(ctx, 0);
}

void ror_printf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const std::string s = ror::format(rdram, ctx, ror::arg(ctx, 0), 1);
    ror::log("printf: %s", trimNewlines(s).c_str());
    ror::ret(ctx, static_cast<uint32_t>(s.size()));
}

void ror_rx_printf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const std::string s = ror::format(rdram, ctx, ror::arg(ctx, 0), 1);
    ror::log("rx: %s", trimNewlines(s).c_str());
    ror::ret(ctx, static_cast<uint32_t>(s.size()));
}

namespace ror
{
    // EE varargs: argument i < 8 is register r4+i (64 bits), the rest are 8-byte slots from the caller's sp. Floats
    // arrive as doubles (soft-float, 64 bits in one register). The stack layout is UNVERIFIED (no >7-argument
    // printf seen yet).
    std::string format(uint8_t *rdram, R5900Context *ctx, uint32_t fmtAddr, int first)
    {
        const std::string fmt = str(rdram, fmtAddr, 2048);
        int next = first;
        auto take64 = [&]() -> uint64_t
        {
            const int i = next++;
            if (i < 8)
                return static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[4 + i]));
            const uint32_t sp = getRegU32(ctx, 29);
            return static_cast<uint64_t>(rd32(rdram, sp + (i - 8) * 8)) |
                   (static_cast<uint64_t>(rd32(rdram, sp + (i - 8) * 8 + 4)) << 32);
        };
        std::string out;
        for (size_t i = 0; i < fmt.size(); ++i)
        {
            if (fmt[i] != '%')
            {
                out.push_back(fmt[i]);
                continue;
            }
            std::string spec = "%";
            size_t j = i + 1;
            while (j < fmt.size() && std::strchr("-+ #0", fmt[j]))
                spec.push_back(fmt[j++]);
            auto number = [&]()
            {
                if (j < fmt.size() && fmt[j] == '*')
                {
                    spec += std::to_string(static_cast<int32_t>(take64()));
                    ++j;
                }
                while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9')
                    spec.push_back(fmt[j++]);
            };
            number();
            if (j < fmt.size() && fmt[j] == '.')
            {
                spec.push_back(fmt[j++]);
                number();
            }
            int longs = 0;
            while (j < fmt.size() && std::strchr("hlqzjt", fmt[j]))
                longs += (fmt[j] == 'l' || fmt[j] == 'q') ? 1 : 0, ++j;
            if (j >= fmt.size())
                break;
            const char c = fmt[j];
            i = j;
            char buf[512];
            buf[0] = 0;
            switch (c)
            {
            case 'd':
            case 'i':
            {
                const uint64_t v = take64();
                if (longs >= 2)
                    std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), static_cast<long long>(v));
                else
                    std::snprintf(buf, sizeof buf, (spec + "d").c_str(), static_cast<int32_t>(v));
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                const uint64_t v = take64();
                if (longs >= 2)
                    std::snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), static_cast<unsigned long long>(v));
                else
                    std::snprintf(buf, sizeof buf, (spec + c).c_str(), static_cast<uint32_t>(v));
                break;
            }
            case 'c':
                std::snprintf(buf, sizeof buf, (spec + "c").c_str(), static_cast<int>(take64() & 0xff));
                break;
            case 'p':
                std::snprintf(buf, sizeof buf, "0x%08x", static_cast<uint32_t>(take64()));
                break;
            case 's':
            {
                const uint32_t a = static_cast<uint32_t>(take64());
                const std::string s = a ? str(rdram, a, 1024) : "(null)";
                std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            {
                const uint64_t bits = take64();
                double d;
                std::memcpy(&d, &bits, sizeof d);
                std::snprintf(buf, sizeof buf, (spec + c).c_str(), d);
                break;
            }
            case '%':
                std::strcpy(buf, "%");
                break;
            default:
                std::snprintf(buf, sizeof buf, "%s%c", spec.c_str(), c);
                break;
            }
            out += buf;
        }
        return out;
    }
}
