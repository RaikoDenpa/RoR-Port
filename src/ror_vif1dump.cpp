// ROR_VIF1_DUMP=F[:N]: the VIF1 stream of frames F..F+N-1 (default N 1) as vif1_<frame>.bin, for tools/port/vif1_chain.py.
// MagSystem::kick_dma 0x1189f0 sends i3r_chain_head(MagSystem + 0x3a0) (0x221120: the head at +0x118) on DMA channel 1
// right after wait_vsync_swap returns, so the chain is complete when wait_vsync_swap ends. It is walked the way the DMAC
// reads a source chain: tag ids refe/cnt/next/ref/refs/call/ret/end, two call levels, and with CHCR.TTE the tag's upper
// 64 bits go into the stream before the data (as PS2Memory's chain path does).
// File: u32 'VIF1', u32 frame, u32 head, u32 tte, then the stream.
#include "ror.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct Range
    {
        uint64_t first = 0, count = 0;
    };

    Range plan()
    {
        Range r;
        if (const char *e = std::getenv("ROR_VIF1_DUMP"))
        {
            char *end = nullptr;
            r.first = std::strtoull(e, &end, 10);
            r.count = (end && *end == ':') ? std::strtoull(end + 1, nullptr, 10) : 1;
        }
        return r;
    }
}

namespace ror
{
    void vif1DumpFrame(uint8_t *rdram, PS2Runtime *runtime, uint32_t magSystem, uint64_t frame)
    {
        static const Range r = plan();
        if (!r.count || frame < r.first || frame >= r.first + r.count)
            return;

        const uint32_t head = rd32(rdram, magSystem + 0x3a0 + 0x118);
        const bool tte = (runtime->memory().readIORegister(0x10009000u) & 0x40u) != 0;
        std::vector<uint8_t> out;
        uint32_t tadr = head, stack[2] = {}, sp = 0;
        int tags = 0, irqTags = 0;
        size_t at4096 = 0; // stream bytes through tag 4096: PS2Recomp's source-chain walk stops there (kMaxChainTags)
        for (bool end = false; !end && tags < 1000000; ++tags)
        {
            const uint8_t *tag = ptr(rdram, tadr, 16);
            if (!tag)
            {
                log("vif1 dump: frame %llu: tag at %08x unreadable", static_cast<unsigned long long>(frame), tadr);
                break;
            }
            uint64_t lo;
            std::memcpy(&lo, tag, 8);
            const uint32_t qwc = static_cast<uint32_t>(lo & 0xffff), id = static_cast<uint32_t>((lo >> 28) & 7);
            const uint32_t addr = static_cast<uint32_t>(lo >> 32) & 0x7ffffff0u;
            irqTags += static_cast<int>((lo >> 31) & 1);
            if (tte)
                out.insert(out.end(), tag + 8, tag + 16);
            uint32_t data = tadr + 16;
            switch (id)
            {
            case 0: data = addr; end = true; break;                          // refe
            case 1: tadr = data + qwc * 16; break;                           // cnt
            case 2: tadr = addr; break;                                      // next
            case 3: case 4: data = addr; tadr += 16; break;                  // ref, refs
            case 5: if (sp < 2) stack[sp++] = data + qwc * 16; tadr = addr; break; // call
            case 6: if (sp) tadr = stack[--sp]; else end = true; break;      // ret
            default: end = true; break;                                      // end
            }
            if (qwc)
            {
                const uint8_t *p = ptr(rdram, data, qwc * 16);
                if (!p)
                {
                    log("vif1 dump: frame %llu: data at %08x unreadable", static_cast<unsigned long long>(frame), data);
                    break;
                }
                out.insert(out.end(), p, p + qwc * 16);
            }
            if (tags == 4095)
                at4096 = out.size();
        }
        const std::string path = "vif1_" + std::to_string(frame) + ".bin";
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            const uint32_t hdr[] = {0x31464956u, static_cast<uint32_t>(frame), head, tte ? 1u : 0u};
            std::fwrite(hdr, sizeof(hdr), 1, f);
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
        }
        log("vif1 dump: frame %llu: chain %08x, %d tags (%d with IRQ), %zu bytes (%zu through tag 4096) -> %s",
            static_cast<unsigned long long>(frame), head, tags, irqTags, out.size(), at4096, path.c_str());
    }
}
