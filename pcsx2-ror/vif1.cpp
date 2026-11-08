// VIF1 + microVU1 (renderer plan step 3, notes/renderer.md 6.5a/6.8): PCSX2's VIF1 (newVif unpackers) and microVU1 on
// the MTVU thread, fed with the port's DMA channel 1 streams; their PATH1/PATH2 go into the same MTGS as the GS feed
// (gs.cpp). The bring-up, the stall loop and the VU1 state record are the gsrunner's -vif1 (tools/pcsx2-patches/0002,
// step 0b): VM memory (VU memories and the recompilers' caches live in it), microVU1 + VIF unpackers, the VU1/VIF1/GIF
// resets; still no VMManager::Initialize, BIOS, EE or IOP. Ordering: an MSCAL puts a placeholder for its XGKICK output
// into the MTGS ring from this (the caller's) thread, so PATH1 stays in order with our PATH3 and VSyncs.
#include "pcsx2/PrecompiledHeader.h"

#include "ror_internal.h"
#include "ror_pcsx2.h"

#include "common/emitter/x86emitter.h"

#include "pcsx2/GS.h"
#include "pcsx2/GS/MultiISA.h"
#include "pcsx2/Gif_Unit.h"
#include "pcsx2/MTVU.h"
#include "pcsx2/Memory.h"
#include "pcsx2/VUmicro.h"
#include "pcsx2/Vif_Dma.h"
#include "pcsx2/Vif_Dynarec.h"

#include <atomic>
#include <cstring>
#include <vector>

using ror_pcsx2::log;

namespace
{
    bool s_open = false;
    std::vector<u32> s_buf;
    std::atomic<unsigned long long> s_streams{0};
    std::atomic<unsigned long long> s_stalls{0};

    // Record 0x40 of a VIF1 GS dump (src/ror_gsdump.cpp; ROR_PCSX2_VU1_STATE_SIZE): VU1 micro + data memory, the VIF1
    // registers as PS2Recomp's VIFRegisters (23 words: stat fbrst err mark cycle mode num mask code itops base ofst tops
    // itop top row[4] col[4]), VF[32][4], VI[16], ACC[4], Q, P, I, R, MAC, clip, status.
    constexpr size_t kVifRegsOffset = 0x8000;
    constexpr size_t kVuRegsOffset = kVifRegsOffset + 23 * 4;
    static_assert(kVuRegsOffset + 32 * 16 + 16 * 4 + 16 + 4 * 7 == ROR_PCSX2_VU1_STATE_SIZE);
} // namespace

bool ror_pcsx2::vif1Open()
{
    if (s_open)
        return true;
    // As VMManager::Internal::CPUThreadInitialize: the emitter has to know about AVX, or microVU's block compare
    // (generated for AVX2 with ymm registers) is encoded without VEX, never matches and microVU recompiles forever.
    x86Emitter::use_avx = g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX;
    if (!SysMemory::Allocate())
    {
        log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: SysMemory::Allocate failed (VIF1/VU1 need PCSX2's VM memory map)");
        return false;
    }
    CpuMicroVU1.Reserve(); // also opens the MTVU thread
    VifUnpackSSE_Init();
    SysMemory::Reset();
    CpuVU1 = &CpuMicroVU1;
    CpuVU1->Reset();
    dVifReset(1);
    vif1Reset();
    gifUnit.Reset();
    vu1Thread.Reset();
    s_streams.store(0);
    s_stalls.store(0);
    s_open = true;
    log(ROR_PCSX2_LOG_INFO, "pcsx2-ror: VIF1 + microVU1 ready (MTVU thread %s, AVX %s)", vu1Thread.IsOpen() ? "open" : "NOT open",
        x86Emitter::use_avx ? "on" : "off");
    return true;
}

void ror_pcsx2::vif1Close()
{
    if (!s_open)
        return;
    // As VMManager::ShutdownCPUProviders + CPUThreadShutdown: without SysMemory::Release the memory map asserts at exit.
    CpuMicroVU1.Shutdown();
    vu1Thread.Close();
    dVifRelease(1);
    SysMemory::Release();
    s_open = false;
}

bool ror_pcsx2::vif1IsOpen()
{
    return s_open;
}

void ror_pcsx2::vif1Counters(unsigned long long &streams, unsigned long long &stalls)
{
    streams = s_streams.load(std::memory_order_relaxed);
    stalls = s_stalls.load(std::memory_order_relaxed);
}

// One stream as DMA channel 1 delivers it. A stall (FLUSH/FLUSHA waiting for the GIF paths or the VU, the -vif1
// loop's 639 per 3 frames in evt1new20) ends VIF1transfer early with the rest in vif1.vifpacketsize: wait for the VU,
// clear the stall, continue.
extern "C" int ror_pcsx2_vif1_transfer(const void *data, size_t size)
{
    if (!s_open)
        return ROR_PCSX2_E_STATE;
    if (!data || size == 0 || size % 4 || size > 0x10000000u)
        return ROR_PCSX2_E_ARG;
    s_buf.resize(size / 4 + 4);
    std::memcpy(s_buf.data(), data, size);
    u32 *p = s_buf.data();
    int words = static_cast<int>(size / 4);
    vif1ch.chcr.STR = true; // lets a FLUSH stall instead of retrying forever
    for (int guard = 0; words > 0; guard++)
    {
        VIF1transfer(p, words, true);
        const int left = static_cast<int>(vif1.vifpacketsize);
        p += words - left;
        words = left;
        if (words == 0)
            break;
        s_stalls.fetch_add(1, std::memory_order_relaxed);
        vu1Thread.WaitVU();
        vif1.vifstalled.enabled = false;
        vif1.waitforvu = false;
        vif1Regs.stat.VGW = false;
        if (guard > 100000)
        {
            log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: VIF1 stream stuck with %d words left (VIFcode %02x)", words, vif1.cmd);
            vif1ch.chcr.STR = false;
            return ROR_PCSX2_E_FAILED;
        }
    }
    vif1ch.chcr.STR = false;
    s_streams.fetch_add(1, std::memory_order_relaxed);
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_vu1_wait(void)
{
    if (!s_open)
        return ROR_PCSX2_E_STATE;
    vu1Thread.WaitVU();
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_vu1_get_state(void *buf, size_t size)
{
    if (!s_open)
        return ROR_PCSX2_E_STATE;
    if (!buf)
        return ROR_PCSX2_E_ARG;
    if (size < ROR_PCSX2_VU1_STATE_SIZE)
        return ROR_PCSX2_E_BUFFER;
    vu1Thread.WaitVU(); // the VU thread owns VU1's memory and registers while it runs
    u8 *s = static_cast<u8 *>(buf);
    std::memcpy(s, VU1.Micro, 0x4000);
    std::memcpy(s + 0x4000, VU1.Mem, 0x4000);
    // STROW/STCOL land in vif1.MaskRow/MaskCol (Vif_Codes.cpp vifCode_STRow/STCol), not in vif1Regs.r0-3/c0-3.
    const u32 vr[23] = {vif1Regs.stat._u32, vif1Regs.fbrst, vif1Regs.err._u32, vif1Regs.mark,
                        static_cast<u32>(vif1Regs.cycle.cl) | (static_cast<u32>(vif1Regs.cycle.wl) << 8), vif1Regs.mode,
                        vif1Regs.num, vif1Regs.mask, vif1Regs.code, vif1Regs.itops, vif1Regs.base, vif1Regs.ofst,
                        vif1Regs.tops, vif1Regs.itop, vif1Regs.top, vif1.MaskRow._u32[0], vif1.MaskRow._u32[1],
                        vif1.MaskRow._u32[2], vif1.MaskRow._u32[3], vif1.MaskCol._u32[0], vif1.MaskCol._u32[1],
                        vif1.MaskCol._u32[2], vif1.MaskCol._u32[3]};
    std::memcpy(s + kVifRegsOffset, vr, sizeof(vr));
    u8 *r = s + kVuRegsOffset;
    for (int i = 0; i < 32; i++, r += 16)
        std::memcpy(r, VU1.VF[i].F, 16);
    for (int i = 0; i < 16; i++, r += 4)
        std::memcpy(r, &VU1.VI[i].UL, 4);
    std::memcpy(r, VU1.ACC.F, 16);
    r += 16;
    std::memcpy(r, &VU1.VI[REG_Q].F, 4);
    std::memcpy(r + 4, &VU1.VI[REG_P].F, 4);
    std::memcpy(r + 8, &VU1.VI[REG_I].F, 4);
    std::memcpy(r + 12, &VU1.VI[REG_R].UL, 4);
    std::memcpy(r + 16, &VU1.VI[REG_MAC_FLAG].UL, 4);
    std::memcpy(r + 20, &VU1.VI[REG_CLIP_FLAG].UL, 4);
    std::memcpy(r + 24, &VU1.VI[REG_STATUS_FLAG].UL, 4);
    return ROR_PCSX2_OK;
}

// The -vif1 loader (patch 0002 LoadVu1State): into PCSX2's structures, then through the VU thread as mtvuFreeze does.
extern "C" int ror_pcsx2_vu1_set_state(const void *buf, size_t size)
{
    if (!s_open)
        return ROR_PCSX2_E_STATE;
    if (!buf || size != ROR_PCSX2_VU1_STATE_SIZE)
        return ROR_PCSX2_E_ARG;
    vu1Thread.WaitVU();
    const u8 *s = static_cast<const u8 *>(buf);
    std::memcpy(VU1.Micro, s, 0x4000);
    std::memcpy(VU1.Mem, s + 0x4000, 0x4000);
    u32 vr[23];
    std::memcpy(vr, s + kVifRegsOffset, sizeof(vr));
    vif1Regs.cycle.cl = static_cast<u8>(vr[4]);
    vif1Regs.cycle.wl = static_cast<u8>(vr[4] >> 8);
    vif1Regs.mode = vr[5];
    vif1Regs.mask = vr[7];
    vif1Regs.itops = vr[9];
    vif1Regs.base = vr[10];
    vif1Regs.ofst = vr[11];
    vif1Regs.tops = vr[12];
    vif1Regs.itop = vr[13];
    vif1Regs.top = vr[14];
    vif1Regs.r0 = vr[15], vif1Regs.r1 = vr[16], vif1Regs.r2 = vr[17], vif1Regs.r3 = vr[18];
    vif1Regs.c0 = vr[19], vif1Regs.c1 = vr[20], vif1Regs.c2 = vr[21], vif1Regs.c3 = vr[22];
    vif1Regs.stat.DBF = (vr[0] >> 7) & 1;
    std::memcpy(&vif1.MaskRow, &vr[15], 16);
    std::memcpy(&vif1.MaskCol, &vr[19], 16);

    const u8 *r = s + kVuRegsOffset;
    for (int i = 0; i < 32; i++, r += 16)
        std::memcpy(VU1.VF[i].F, r, 16);
    for (int i = 0; i < 16; i++, r += 4)
        std::memcpy(&VU1.VI[i].UL, r, 4);
    std::memcpy(VU1.ACC.F, r, 16);
    r += 16;
    std::memcpy(&VU1.VI[REG_Q].F, r, 4);
    std::memcpy(&VU1.VI[REG_P].F, r + 4, 4);
    std::memcpy(&VU1.VI[REG_I].F, r + 8, 4);
    std::memcpy(&VU1.VI[REG_R].UL, r + 12, 4);
    std::memcpy(&VU1.VI[REG_MAC_FLAG].UL, r + 16, 4);
    std::memcpy(&VU1.VI[REG_CLIP_FLAG].UL, r + 20, 4);
    std::memcpy(&VU1.VI[REG_STATUS_FLAG].UL, r + 24, 4);

    vu1Thread.Reset();
    vu1Thread.WriteCol(vif1);
    vu1Thread.WriteRow(vif1);
    vu1Thread.WriteMicroMem(0, VU1.Micro, 0x4000);
    vu1Thread.WriteDataMem(0, VU1.Mem, 0x4000);
    vu1Thread.WriteVIRegs(&VU1.VI[0]);
    vu1Thread.WriteVFRegs(&VU1.VF[0]);
    return ROR_PCSX2_OK;
}
