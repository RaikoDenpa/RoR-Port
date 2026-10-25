// PCSX2 GS dumps from the runtime's GIF side, in the format of the PCSX2 version the renderer plan pins (v2.8.2:
// pcsx2/GS/GSDump.cpp, GSState::Freeze state version 9, GSLzma.cpp GSDumpFile::ReadFile, GSDumpReplayer.cpp), so a
// captured frame replays in PCSX2's GS (notes/renderer.md).
//
//   ROR_GSDUMP=F[:N][,F[:N]...]   gsdump_F.gs: the GS state at the end of wait_vsync_swap F, then N frames (default 3)
//                                  of GIF transfers, GS->host reads, privileged registers and vsyncs
//
//   transfers   GifArbiter observer (patch 0008): each packet as the GS gets it, per path, without the GIFtags the VIF1
//               interpreter adds in front of IMAGE data that continues in a later DIRECT. Dump path = the index of
//               GSState::Transfer<index>: PATH2 1, PATH3 2, PATH1 (XGKICK, whole packets) 3 "Path1New"
//   A+D         GS::writeRegister observer: register writes runtime handlers make outside the GIF stream
//               (sceGsPutDrawEnv, from gs_swap_env 0x25b908 every frame) -> a one-register A+D packet on PATH3
//   reads       ConsumeLocalToHostBytes (sceGsExecStoreImage) -> ReadFIFO2, size in quadwords
//   vsyncs      every VBlank the host clock runs (ror_vsync.cpp, on the game thread): the privileged registers
//               (GSPrivRegSet: the 8 KB at 0x12000000, CSR.FIELD alternating), then VSync
//   state       GSState::Freeze from a shadow of every GS register write since boot (each path's GIF stream parsed,
//               PACKED data converted to register values as the GS does) and the GS memory; transfer state idle (total
//               0, what PCSX2 assumes for version 8 dumps), path tags idle. A dump starts only when no path is inside a
//               GIF packet, otherwise one frame later.
#include "ror.h"

#include "runtime/gs/ps2_gif_arbiter.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kStateVersion = 9;   // GSState::STATE_VERSION
    constexpr uint32_t kCrc = 0x52585249u;  // ElfObject::GetCRC (XOR of all words) of SLES_542.18
    constexpr char kSerial[] = "SLES-54218";
    constexpr uint32_t kStateSize = 4194813u; // GSState::GetSaveStateSize(9)
    enum : uint8_t { kTransfer = 0, kVSync = 1, kReadFifo2 = 2, kRegisters = 3 };  // GSDumpTypes::GSType
    enum : uint8_t { kPath2 = 1, kPath3 = 2, kPath1New = 3 };                        // GSDumpTypes::GSTransferPath
    constexpr uint64_t kTex2Mask = 0xFFFFFFE003F00000ull;                           // GSState::GIFRegHandlerTEX2

    // One GIF path: what is left of the current GIFtag (PACKED: quadwords, REGLIST: 64-bit words, IMAGE: quadwords).
    struct PathState
    {
        uint32_t left = 0;
        uint8_t flg = 0;
        uint32_t nreg = 0;
        uint64_t regs = 0;
        uint32_t index = 0;
        bool pad = false; // REGLIST with an odd word count: the last quadword's upper half follows
    };

    // GS register state as PCSX2 keeps it (m_env, m_v, m_q), indexed by register address.
    struct Shadow
    {
        uint64_t reg[0x64] = {};
        uint64_t prim = 0;
        uint64_t rgbaq = 0, st = 0, xyz = 0;
        uint32_t uv = 0, fog = 0;
        uint32_t q = 0x3f800000u; // 1.0f
        PathState path[4];        // by dump path
    } g_s;

    struct Planned
    {
        uint64_t frame;
        uint32_t frames;
        bool done;
    };

    std::mutex g_mutex;
    PS2Runtime *g_runtime = nullptr;
    std::vector<Planned> g_plan;
    FILE *g_out = nullptr;
    std::string g_path;
    uint32_t g_framesLeft = 0;
    bool g_field = false;
    uint32_t g_count[4] = {}, g_regWrites = 0, g_vsyncs = 0, g_reads = 0;

    void writeReg(uint8_t a, uint64_t v)
    {
        switch (a)
        {
        case 0x00: // PRIM: all of it with PRMODECONT.AC = 1, else only the primitive type (GSState::ApplyPRIM)
            g_s.prim = (g_s.reg[0x1a] & 1) ? (v & 0x7ff) : ((g_s.prim & ~7ull) | (v & 7));
            break;
        case 0x1b: // PRMODE: the attributes, with PRMODECONT.AC = 0
            if (!(g_s.reg[0x1a] & 1))
                g_s.prim = (v & 0x7f8) | (g_s.prim & 7);
            break;
        case 0x01: g_s.rgbaq = v; break;
        case 0x02: g_s.st = v; break;
        case 0x03: g_s.uv = static_cast<uint32_t>(v & 0x3fff3fffu); break;
        case 0x04:
        case 0x0c:
            g_s.xyz = v & 0x00ffffffffffffffull;
            g_s.fog = static_cast<uint32_t>(v >> 56);
            break;
        case 0x05:
        case 0x0d: g_s.xyz = v; break;
        case 0x0a: g_s.fog = static_cast<uint32_t>(v >> 56); break;
        case 0x16:
        case 0x17: // TEX2_1/2: the CLUT fields of TEX0_1/2
            g_s.reg[a - 0x10] = (g_s.reg[a - 0x10] & ~kTex2Mask) | (v & kTex2Mask);
            break;
        default:
            if (a < 0x64)
                g_s.reg[a] = v;
            break;
        }
    }

    // PACKED data -> the register write the GS makes of it.
    void writePacked(uint8_t desc, uint64_t lo, uint64_t hi)
    {
        switch (desc)
        {
        case 0x0: writeReg(0x00, lo & 0x7ff); break;
        case 0x1:
            writeReg(0x01, (lo & 0xff) | ((lo >> 24) & 0xff00) | ((hi & 0xff) << 16) | ((hi >> 8) & 0xff000000ull) |
                               (static_cast<uint64_t>(g_s.q) << 32));
            break;
        case 0x2:
            g_s.q = static_cast<uint32_t>(hi);
            writeReg(0x02, lo);
            break;
        case 0x3: writeReg(0x03, (lo & 0x3fff) | (((lo >> 32) & 0x3fff) << 16)); break;
        case 0x4:
        case 0xc:
            writeReg(desc == 0xc || ((hi >> 47) & 1) ? 0x0c : 0x04,
                     (lo & 0xffff) | (((lo >> 32) & 0xffff) << 16) | (((hi >> 4) & 0xffffff) << 32) | (((hi >> 36) & 0xff) << 56));
            break;
        case 0x5:
        case 0xd:
            writeReg(desc == 0xd || ((hi >> 47) & 1) ? 0x0d : 0x05,
                     (lo & 0xffff) | (((lo >> 32) & 0xffff) << 16) | ((hi & 0xffffffffull) << 32));
            break;
        case 0xa: writeReg(0x0a, ((hi >> 36) & 0xff) << 56); break;
        case 0xe: writeReg(static_cast<uint8_t>(hi & 0xff), lo); break;
        case 0xf: break;
        default: writeReg(desc, lo); break;
        }
    }

    void consume(int path, const uint8_t *p, uint32_t n)
    {
        PathState &s = g_s.path[path];
        uint32_t pos = 0;
        while (pos < n)
        {
            if (s.left == 0)
            {
                if (s.pad)
                {
                    pos += 8;
                    s.pad = false;
                    continue;
                }
                if (pos + 16 > n)
                    break;
                uint64_t lo, hi;
                std::memcpy(&lo, p + pos, 8);
                std::memcpy(&hi, p + pos + 8, 8);
                pos += 16;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7fff);
                s.flg = static_cast<uint8_t>((lo >> 58) & 3);
                s.nreg = static_cast<uint32_t>((lo >> 60) & 0xf);
                if (s.nreg == 0)
                    s.nreg = 16;
                s.regs = hi;
                s.index = 0;
                if (s.flg == 0 && ((lo >> 46) & 1)) // PRE: the tag's PRIM field goes to PRIM
                    writeReg(0x00, (lo >> 47) & 0x7ff);
                s.left = s.flg >= 2 ? nloop : nloop * s.nreg;
                s.pad = s.flg == 1 && (s.left & 1);
                continue;
            }
            if (s.flg == 0) // PACKED
            {
                if (pos + 16 > n)
                    break;
                uint64_t lo, hi;
                std::memcpy(&lo, p + pos, 8);
                std::memcpy(&hi, p + pos + 8, 8);
                writePacked(static_cast<uint8_t>((s.regs >> (4 * s.index)) & 0xf), lo, hi);
                pos += 16;
            }
            else if (s.flg == 1) // REGLIST
            {
                if (pos + 8 > n)
                    break;
                uint64_t v;
                std::memcpy(&v, p + pos, 8);
                const uint8_t desc = static_cast<uint8_t>((s.regs >> (4 * s.index)) & 0xf);
                if (desc < 0xe)
                    writeReg(desc, v);
                pos += 8;
            }
            else // IMAGE (FLG 2 or 3): the data itself is in the GS memory snapshot
            {
                const uint32_t k = std::min(s.left, (n - pos) / 16);
                if (k == 0)
                    break;
                pos += k * 16;
                s.left -= k;
                continue;
            }
            --s.left;
            s.index = (s.index + 1) % s.nreg;
        }
    }

    template <typename T>
    void put(std::vector<uint8_t> &v, const T &x)
    {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&x);
        v.insert(v.end(), b, b + sizeof(T));
    }

    // GSState::Freeze, version 9.
    std::vector<uint8_t> freeze(const std::vector<uint8_t> &vram)
    {
        std::vector<uint8_t> s;
        s.reserve(kStateSize);
        put(s, kStateVersion);
        put(s, g_s.prim);
        for (uint8_t a : {0x1a, 0x1c, 0x22, 0x3b, 0x3d, 0x44, 0x45, 0x46, 0x49, 0x50, 0x53, 0x51, 0x52, 0x52})
            put(s, g_s.reg[a]); // PRMODECONT TEXCLUT SCANMSK TEXA FOGCOL DIMX DTHE COLCLAMP PABE BITBLTBUF TRXDIR TRXPOS TRXREG TRXREG
        for (uint8_t i = 0; i < 2; ++i)
            for (uint8_t a : {0x18, 0x06, 0x14, 0x08, 0x34, 0x36, 0x40, 0x42, 0x47, 0x4a, 0x4c, 0x4e})
                put(s, g_s.reg[a + i]); // XYOFFSET TEX0 TEX1 CLAMP MIPTBP1 MIPTBP2 SCISSOR ALPHA TEST FBA FRAME ZBUF
        put(s, g_s.rgbaq);
        put(s, g_s.st);
        put(s, g_s.uv);
        put(s, g_s.fog);
        put(s, g_s.xyz);
        put(s, uint64_t{0}); // obsolete
        const uint64_t trxpos = g_s.reg[0x51], trxreg = g_s.reg[0x52];
        const int32_t w = static_cast<int32_t>(trxreg & 0xfff), h = static_cast<int32_t>((trxreg >> 32) & 0xfff);
        const int32_t dx = static_cast<int32_t>((trxpos >> 32) & 0x7ff), dy = static_cast<int32_t>((trxpos >> 48) & 0x7ff);
        put(s, int32_t{0}); // m_tr.x, y
        put(s, int32_t{0});
        put(s, w);
        put(s, h);
        put(s, g_s.reg[0x50]); // m_blit, m_pos, m_reg
        put(s, trxpos);
        put(s, trxreg);
        for (int32_t r : {dx, dy, dx + w, dy + h})
            put(s, r);
        put(s, int32_t{0}); // total, start, end: no transfer in progress
        put(s, int32_t{0});
        put(s, int32_t{0});
        put(s, uint8_t{1}); // write
        s.insert(s.end(), vram.begin(), vram.end());
        for (int i = 0; i < 4; ++i) // GIFPath tag and reg: idle
        {
            s.insert(s.end(), 16, 0);
            put(s, uint32_t{0});
        }
        put(s, g_s.q);
        return s;
    }

    // GSPrivRegSet: the 8 KB at 0x12000000, registers 16 bytes apart.
    std::vector<uint8_t> privRegs(bool field)
    {
        std::vector<uint8_t> r(8192, 0);
        const GSRegisters &g = g_runtime->memory().gs();
        auto at = [&](uint32_t off, uint64_t v) { std::memcpy(r.data() + off, &v, 8); };
        at(0x00, g.pmode);
        at(0x10, g.smode1);
        at(0x20, g.smode2);
        at(0x30, g.srfsh);
        at(0x40, g.synch1);
        at(0x50, g.synch2);
        at(0x60, g.syncv);
        at(0x70, g.dispfb1);
        at(0x80, g.display1);
        at(0x90, g.dispfb2);
        at(0xa0, g.display2);
        at(0xb0, g.extbuf);
        at(0xc0, g.extdata);
        at(0xd0, g.extwrite);
        at(0xe0, g.bgcolor);
        const uint64_t csr = g.csr.load(std::memory_order_relaxed);
        at(0x1000, field ? (csr | 0x2000u) : (csr & ~0x2000ull)); // CSR.FIELD: MTGS passes field = FIELD ? 0 : 1
        at(0x1010, g.imr);
        at(0x1040, g.busdir);
        at(0x1080, g.siglblid);
        return r;
    }

    void writeTransfer(uint8_t path, const uint8_t *data, uint32_t size)
    {
        std::fputc(kTransfer, g_out);
        std::fputc(path, g_out);
        std::fwrite(&size, 4, 1, g_out);
        std::fwrite(data, 1, size, g_out);
        ++g_count[path];
    }

    bool busy()
    {
        for (const PathState &p : g_s.path)
            if (p.left || p.pad)
                return true;
        return false;
    }

    void start(uint64_t frame, uint32_t frames)
    {
        g_path = "gsdump_" + std::to_string(frame) + ".gs";
        g_out = std::fopen(g_path.c_str(), "wb");
        if (!g_out)
        {
            ror::log("gs dump: cannot write %s", g_path.c_str());
            return;
        }
        std::vector<uint8_t> vram;
        ror::gsSnapshotVram(vram);
        vram.resize(4u << 20);
        const std::vector<uint8_t> state = freeze(vram);
        if (state.size() != kStateSize)
            ror::log("gs dump: state is %zu bytes, PCSX2 expects %u", state.size(), kStateSize);

        const uint32_t serialSize = sizeof(kSerial) - 1;
        const uint32_t header[] = {kStateVersion, static_cast<uint32_t>(state.size()), 36, serialSize, kCrc, 0, 0,
                                   36 + serialSize, 0}; // GSDumpHeader: no screenshot
        const uint32_t lead[] = {0xffffffffu, static_cast<uint32_t>(sizeof(header)) + serialSize};
        std::fwrite(lead, sizeof(lead), 1, g_out);
        std::fwrite(header, sizeof(header), 1, g_out);
        std::fwrite(kSerial, 1, serialSize, g_out);
        std::fwrite(state.data(), 1, state.size(), g_out);
        const std::vector<uint8_t> regs = privRegs(g_field);
        std::fwrite(regs.data(), 1, regs.size(), g_out);

        g_framesLeft = frames;
        std::memset(g_count, 0, sizeof(g_count));
        g_regWrites = g_vsyncs = g_reads = 0;
        ror::log("gs dump: frame %llu -> %s (%u frames)", static_cast<unsigned long long>(frame), g_path.c_str(), frames);
    }

    void finish()
    {
        const long size = std::ftell(g_out);
        std::fclose(g_out);
        g_out = nullptr;
        ror::log("gs dump: %s: %u PATH1, %u PATH2, %u PATH3 transfers (%u of them A+D from writeRegister), %u vsyncs, "
                 "%u reads, %ld bytes",
                 g_path.c_str(), g_count[kPath1New], g_count[kPath2], g_count[kPath3], g_regWrites, g_vsyncs, g_reads, size);
    }
}

namespace ror
{
    void gsDumpInstall(PS2Runtime &runtime)
    {
        const char *e = std::getenv("ROR_GSDUMP");
        for (const char *p = e; p && *p;)
        {
            char *end = nullptr;
            Planned d{std::strtoull(p, &end, 10), 3, false};
            if (end == p)
                break;
            if (*end == ':')
                d.frames = static_cast<uint32_t>(std::strtoul(end + 1, &end, 10));
            g_plan.push_back(d);
            p = *end == ',' ? end + 1 : end;
        }
        if (g_plan.empty())
            return;
        g_runtime = &runtime;
        runtime.gifArbiter().setObserver([](GifPathId id, const uint8_t *data, uint32_t size)
                                         {
            const uint8_t path = id == GifPathId::Path1 ? kPath1New : id == GifPathId::Path2 ? kPath2 : kPath3;
            std::lock_guard<std::mutex> lock(g_mutex);
            consume(path, data, size);
            if (g_out)
                writeTransfer(path, data, size); });
        runtime.gs().setRegisterObserver([](uint8_t reg, uint64_t value)
                                         {
            std::lock_guard<std::mutex> lock(g_mutex);
            writeReg(reg, value);
            if (!g_out)
                return;
            if (g_s.path[kPath3].left)
                ror::log("gs dump: register %02x written while PATH3 is inside a packet", reg);
            const uint64_t packet[4] = {1ull | (1ull << 15) | (1ull << 60), 0xe, value, reg}; // NLOOP 1, EOP, PACKED, A+D
            writeTransfer(kPath3, reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
            ++g_regWrites; });
        ror::log("gs dump: %zu planned, GIF and register observers installed", g_plan.size());
    }

    void gsDumpFrameMark(uint64_t frame)
    {
        if (g_plan.empty())
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_out)
        {
            if (--g_framesLeft == 0)
                finish();
            return;
        }
        for (Planned &d : g_plan)
            if (!d.done && frame >= d.frame)
            {
                if (busy())
                {
                    ror::log("gs dump: a GIF path is inside a packet at frame %llu, trying the next frame",
                             static_cast<unsigned long long>(frame));
                    return;
                }
                d.done = true;
                start(frame, d.frames ? d.frames : 1);
                return;
            }
    }

    void gsDumpVsync()
    {
        if (g_plan.empty())
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_field = !g_field;
        if (!g_out)
            return;
        const std::vector<uint8_t> regs = privRegs(g_field);
        std::fputc(kRegisters, g_out);
        std::fwrite(regs.data(), 1, regs.size(), g_out);
        std::fputc(kVSync, g_out);
        std::fputc(g_field ? 0 : 1, g_out);
        ++g_vsyncs;
    }

    void gsDumpReadFifo(uint32_t bytes)
    {
        if (g_plan.empty() || bytes == 0)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_out)
            return;
        const uint32_t qwords = (bytes + 15) / 16;
        std::fputc(kReadFifo2, g_out);
        std::fwrite(&qwords, 4, 1, g_out);
        ++g_reads;
    }
}
