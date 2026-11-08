// The GS seam and its diagnostics: a raster backend that wraps PS2Recomp's GSCpuBackend (ROR_GS=cpu) or a null one
// (ROR_GS=pcsx2: PCSX2's VIF1 + microVU1 + GS render, fed from here with DMA channel 1's streams, PATH3 and each VBlank,
// presenting into a child window instead of raylib)
// (installed with GS::setRasterBackend, which supports hot replacement), times the backend calls made on the game
// thread, and for one frame (ROR_GS_TRACE=N)
// writes every draw, CLUT load and transfer with its decoded GS state, plus the privileged display registers, to
// ROR_GS_TRACE_OUT (default gs_frame_N.tsv). A "frame" is the span from the end of MagSystem::wait_vsync_swap N to
// the start of N+1: kick_dma sends frame N's chain there (MagSystem::frame 0x11848c), followed by frame N+1's
// immediate texture uploads. tools/port/gs_trace.py summarises the file.
// ROR_SCREENSHOT=F[,F...] saves the presented frame (what the window shows) at the end of wait_vsync_swap F as
// screen_F.png.
#include "ror.h"
#include "ror_pcsx2host.h"

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    std::atomic<uint64_t> g_backendNs{0};
    std::thread::id g_gameThread;
    FILE *g_out = nullptr;
    uint64_t g_traceFrame = 0;
    uint32_t g_draws = 0;
    std::vector<uint64_t> g_shotFrames;

    const char *primName(GSPrimType t)
    {
        static const char *names[] = {"point", "line", "linestrip", "triangle", "tristrip", "trifan", "sprite", "prim7"};
        return names[static_cast<unsigned>(t) & 7u];
    }

    const char *psmName(uint8_t psm)
    {
        switch (psm)
        {
        case 0x00: return "CT32";
        case 0x01: return "CT24";
        case 0x02: return "CT16";
        case 0x0a: return "CT16S";
        case 0x13: return "T8";
        case 0x14: return "T4";
        case 0x1b: return "T8H";
        case 0x24: return "T4HL";
        case 0x2c: return "T4HH";
        case 0x30: return "Z32";
        case 0x31: return "Z24";
        case 0x32: return "Z16";
        case 0x3a: return "Z16S";
        default: return "?";
        }
    }

    void writeDraw(const GSPrimitiveBatch &b)
    {
        const GSDrawState &s = b.state;
        const GSContext &c = s.context;
        const float ox = c.xyoffset.ofx / 16.0f, oy = c.xyoffset.ofy / 16.0f;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        int amin = 255, amax = 0;
        for (int i = 0; i < b.vertexCount; ++i)
        {
            const GSVertex &v = b.vertices[i];
            x0 = std::min(x0, v.x - ox), x1 = std::max(x1, v.x - ox);
            y0 = std::min(y0, v.y - oy), y1 = std::max(y1, v.y - oy);
            amin = std::min<int>(amin, v.a), amax = std::max<int>(amax, v.a);
        }
        const GSVertex &v0 = b.vertices[0];
        const uint64_t t = c.test, a = c.alpha, t1 = c.tex1;
        std::fprintf(g_out,
                     "draw\t%u\tprim=%s\tctxt=%d\tiip=%d\ttme=%d\tfge=%d\tabe=%d\taa1=%d\tfst=%d\tverts=%u"
                     "\tx0=%.1f\ty0=%.1f\tx1=%.1f\ty1=%.1f\trgba0=%02x%02x%02x%02x\tamin=%02x\tamax=%02x"
                     "\tfbp=%u\tfbw=%u\tfpsm=%s\tfbmsk=%08x\tzbp=%u\tzpsm=%s\tzmsk=%d"
                     "\tate=%u\tatst=%u\taref=%02x\tafail=%u\tdate=%u\tdatm=%u\tzte=%u\tztst=%u"
                     "\talpha_a=%u\talpha_b=%u\talpha_c=%u\talpha_d=%u\tfix=%02x\tpabe=%d\tfba=%u"
                     "\ttbp0=%u\ttbw=%u\ttpsm=%s\ttw=%u\tth=%u\ttcc=%u\ttfx=%u\tcbp=%u\tcpsm=%s\tcsm=%u\tcsa=%u\tcld=%u"
                     "\tmmag=%u\tmmin=%u\tclamp=%llx\tta0=%02x\taem=%d\tta1=%02x\tcbw=%u\tcou=%u\tcov=%u"
                     "\tdthe=%llu\tdimx=%llx\tcolclamp=%llu\tscissor=%u,%u,%u,%u\n",
                     g_draws++, primName(s.prim.type), s.prim.ctxt, s.prim.iip, s.prim.tme, s.prim.fge, s.prim.abe,
                     s.prim.aa1, s.prim.fst, b.vertexCount, x0, y0, x1, y1, v0.r, v0.g, v0.b, v0.a, amin, amax,
                     c.frame.fbp, c.frame.fbw, psmName(c.frame.psm), c.frame.fbmsk, c.zbuf.zbp, psmName(c.zbuf.psm),
                     c.zbuf.zmask, static_cast<unsigned>(t & 1), static_cast<unsigned>((t >> 1) & 7),
                     static_cast<unsigned>((t >> 4) & 0xff), static_cast<unsigned>((t >> 12) & 3),
                     static_cast<unsigned>((t >> 14) & 1), static_cast<unsigned>((t >> 15) & 1),
                     static_cast<unsigned>((t >> 16) & 1), static_cast<unsigned>((t >> 17) & 3),
                     static_cast<unsigned>(a & 3), static_cast<unsigned>((a >> 2) & 3),
                     static_cast<unsigned>((a >> 4) & 3), static_cast<unsigned>((a >> 6) & 3),
                     static_cast<unsigned>((a >> 32) & 0xff), s.pabe, static_cast<unsigned>(c.fba & 1),
                     c.tex0.tbp0, c.tex0.tbw, psmName(c.tex0.psm), c.tex0.tw, c.tex0.th, c.tex0.tcc, c.tex0.tfx,
                     c.tex0.cbp, psmName(c.tex0.cpsm), c.tex0.csm, c.tex0.csa, c.tex0.cld,
                     static_cast<unsigned>((t1 >> 5) & 1), static_cast<unsigned>((t1 >> 6) & 7),
                     static_cast<unsigned long long>(c.clamp), s.texa.ta0, s.texa.aem, s.texa.ta1, s.texclut.cbw,
                     s.texclut.cou, s.texclut.cov, static_cast<unsigned long long>(s.dthe),
                     static_cast<unsigned long long>(s.dimx), static_cast<unsigned long long>(s.colclamp),
                     c.scissor.x0, c.scissor.y0, c.scissor.x1, c.scissor.y1);
    }

    class TracingBackend final : public GSRasterBackend
    {
    public:
        explicit TracingBackend(std::unique_ptr<GSRasterBackend> inner) : m_inner(std::move(inner)) {}

        void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
        void Reset() override { m_inner->Reset(); }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            if (g_out)
                writeDraw(batch);
            timed([&] { m_inner->Submit(batch); });
        }

        void LoadClut(const GSTex0Reg &t, const GSTexClutReg &c) override
        {
            if (g_out && t.cld != 0)
                std::fprintf(g_out, "clut\tafter_draw=%u\tpsm=%s\tcbp=%u\tcpsm=%s\tcsm=%u\tcsa=%u\tcld=%u\tcbw=%u\tcou=%u\tcov=%u\n",
                             g_draws, psmName(t.psm), t.cbp, psmName(t.cpsm), t.csm, t.csa, t.cld, c.cbw, c.cou, c.cov);
            timed([&] { m_inner->LoadClut(t, c); });
        }

        void BeginTransfer(const GSTransferCommand &x) override
        {
            // Every GS->host read is logged (renderer plan: the readback measurement before step 4).
            if (x.direction == 1u)
                ror::log("GS->host read: %ux%u %s from %u (BITBLTBUF.SBP), frame %llu", x.trxreg.rrw, x.trxreg.rrh,
                         psmName(x.bitbltbuf.spsm), x.bitbltbuf.sbp, static_cast<unsigned long long>(ror::frame()));
            if (g_out)
                std::fprintf(g_out, "transfer\tafter_draw=%u\tdbp=%u\tdbw=%u\tdpsm=%s\tsbp=%u\tdsax=%u\tdsay=%u\trrw=%u\trrh=%u\tdir=%u\n",
                             g_draws, x.bitbltbuf.dbp, x.bitbltbuf.dbw, psmName(x.bitbltbuf.dpsm), x.bitbltbuf.sbp,
                             x.trxpos.dsax, x.trxpos.dsay, x.trxreg.rrw, x.trxreg.rrh, x.direction);
            timed([&] { m_inner->BeginTransfer(x); });
        }

        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override { timed([&] { m_inner->UploadImage(data, sizeBytes); }); }
        void Flush() override { timed([&] { m_inner->Flush(); }); }
        void TextureFlush() override { timed([&] { m_inner->TextureFlush(); }); }
        void Sync(GSSyncReason reason) override { timed([&] { m_inner->Sync(reason); }); }
        PresentationFrame Present(const GSPresentationRequest &r) override { return m_inner->Present(r); }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            bool ok = false;
            timed([&] { ok = m_inner->ClearFramebuffer(context, rgba); });
            return ok;
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            const uint32_t n = m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
            ror::gsDumpReadFifo(n);
            return n;
        }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            return m_inner->ReadVram(psm, base, bw, x, y);
        }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            m_inner->WriteVram(psm, base, bw, x, y, value);
        }
        void SnapshotVram(std::vector<uint8_t> &out) const override { m_inner->SnapshotVram(out); }
        GSTransferSnapshot GetTransferSnapshot() const override { return m_inner->GetTransferSnapshot(); }

    private:
        template <typename F>
        void timed(F &&f)
        {
            if (std::this_thread::get_id() != g_gameThread)
            {
                f();
                return;
            }
            const auto t0 = Clock::now();
            f();
            g_backendNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()),
                                  std::memory_order_relaxed);
        }

        std::unique_ptr<GSRasterBackend> m_inner;
    };

    // ROR_GS=pcsx2: PS2Recomp's frontend still parses every GIF packet (CSR, IMR and the trace see it all), but nothing
    // is rasterised here; PCSX2's GS renders. A GS->host read (renderer plan step 4) gets PCSX2's pixels: the transfer
    // registers went to PCSX2 on PATH3 with the packet, so the first read fetches the whole image from its GS
    // (MTGS::InitAndReadFIFO, after everything sent before is drawn) and the following ones serve it.
    class NullBackend final : public GSRasterBackend
    {
    public:
        void Initialize(uint8_t *, uint32_t) override {}
        void Reset() override { m_readback = m_readPos = 0; }
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &c) override
        {
            m_readback = m_readPos = 0;
            m_pixels.clear();
            if (c.direction == 1u)
                m_readback = (static_cast<size_t>(c.trxreg.rrw) * c.trxreg.rrh *
                              GSInternal::bitsPerPixel(c.bitbltbuf.spsm) + 7u) / 8u;
        }
        void UploadImage(const uint8_t *, uint32_t) override {}
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return true; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            if (!dst || m_readPos >= m_readback)
                return 0u;
            if (m_pixels.empty())
            {
                m_pixels.assign((m_readback + 15u) & ~static_cast<size_t>(15u), 0);
                if (!ror::pcsx2ReadFifo(m_pixels.data(), m_pixels.size()))
                    ror::log("GS->host read: PCSX2 gave nothing, %zu bytes of zeros", m_readback);
            }
            const size_t n = std::min<size_t>(maxBytes, m_readback - m_readPos);
            std::memcpy(dst, m_pixels.data() + m_readPos, n);
            m_readPos += n;
            return static_cast<uint32_t>(n);
        }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &out) const override { out.assign(4u << 20, 0); }
        GSTransferSnapshot GetTransferSnapshot() const override
        {
            GSTransferSnapshot s;
            s.localToHostPendingBytes = m_readback - m_readPos;
            return s;
        }

    private:
        size_t m_readback = 0, m_readPos = 0;
        std::vector<uint8_t> m_pixels;
    };

    TracingBackend *g_backend = nullptr;
    bool g_pcsx2 = false;   // PCSX2's GS renders (ROR_GS=pcsx2)
    bool g_field = false;   // CSR.FIELD for PCSX2, alternating per VBlank

    // ROR_GS_VRAM=1: the 4 MB of GS local memory next to the trace, at the start and end of the frame
    // (tools/port/gs_vram.py decodes textures and CLUTs from it).
    void dumpVram(const std::string &path)
    {
        const char *e = std::getenv("ROR_GS_VRAM");
        if (!e || *e != '1' || !g_backend)
            return;
        std::vector<uint8_t> vram;
        g_backend->SnapshotVram(vram);
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fwrite(vram.data(), 1, vram.size(), f);
            std::fclose(f);
        }
    }

    void writePrivileged(PS2Runtime *runtime)
    {
        const GSRegisters &r = runtime->memory().gs();
        auto display = [&](const char *n, uint64_t v)
        {
            std::fprintf(g_out, "\t%s=%016llx\t%s_dx=%llu\t%s_dy=%llu\t%s_magh=%llu\t%s_magv=%llu\t%s_dw=%llu\t%s_dh=%llu", n,
                         static_cast<unsigned long long>(v), n, static_cast<unsigned long long>(v & 0xfff), n,
                         static_cast<unsigned long long>((v >> 12) & 0x7ff), n, static_cast<unsigned long long>((v >> 23) & 0xf), n,
                         static_cast<unsigned long long>((v >> 27) & 3), n, static_cast<unsigned long long>((v >> 32) & 0xfff), n,
                         static_cast<unsigned long long>((v >> 44) & 0x7ff));
        };
        auto dispfb = [&](const char *n, uint64_t v)
        {
            std::fprintf(g_out, "\t%s=%016llx\t%s_fbp=%llu\t%s_fbw=%llu\t%s_psm=%s\t%s_dbx=%llu\t%s_dby=%llu", n,
                         static_cast<unsigned long long>(v), n, static_cast<unsigned long long>(v & 0x1ff), n,
                         static_cast<unsigned long long>((v >> 9) & 0x3f), n, psmName(static_cast<uint8_t>((v >> 15) & 0x1f)), n,
                         static_cast<unsigned long long>((v >> 32) & 0x7ff), n, static_cast<unsigned long long>((v >> 43) & 0x7ff));
        };
        const uint64_t pm = r.pmode, sm = r.smode2;
        std::fprintf(g_out, "priv\tpmode=%llx\ten1=%llu\ten2=%llu\tmmod=%llu\tamod=%llu\tslbg=%llu\talp=%02llx"
                            "\tsmode2=%llx\tint=%llu\tffmd=%llu\tdpms=%llu\tbgcolor=%06llx",
                     static_cast<unsigned long long>(pm), static_cast<unsigned long long>(pm & 1),
                     static_cast<unsigned long long>((pm >> 1) & 1), static_cast<unsigned long long>((pm >> 5) & 1),
                     static_cast<unsigned long long>((pm >> 6) & 1), static_cast<unsigned long long>((pm >> 7) & 1),
                     static_cast<unsigned long long>((pm >> 8) & 0xff), static_cast<unsigned long long>(sm),
                     static_cast<unsigned long long>(sm & 1), static_cast<unsigned long long>((sm >> 1) & 1),
                     static_cast<unsigned long long>((sm >> 2) & 3), static_cast<unsigned long long>(r.bgcolor & 0xffffff));
        display("display1", r.display1);
        dispfb("dispfb1", r.dispfb1);
        display("display2", r.display2);
        dispfb("dispfb2", r.dispfb2);
        std::fprintf(g_out, "\n");
    }

    // The frame the render thread latched last (GS::latchHostPresentationFrame, once per vsync tick), opaque. With
    // PCSX2's GS: the next frame it presents, saved by PCSX2 (asynchronously).
    void screenshot(PS2Runtime *runtime, uint64_t frame)
    {
        if (g_pcsx2)
        {
            const std::string path = (std::filesystem::current_path() / ("screen_" + std::to_string(frame) + ".png")).string();
            const bool ok = ror::pcsx2Screenshot(path.c_str());
            ror::log("screenshot: frame %llu -> %s (PCSX2)%s", static_cast<unsigned long long>(frame), path.c_str(),
                     ok ? "" : " (GS not open)");
            return;
        }
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0;
        if (!runtime->gs().copyLatchedHostPresentationFrame(pixels, w, h) || !w || !h)
        {
            ror::log("screenshot: no presented frame at frame %llu", static_cast<unsigned long long>(frame));
            return;
        }
        for (size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 0xff;
        const std::string path = "screen_" + std::to_string(frame) + ".png";
        Image image{pixels.data(), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        const bool ok = ExportImage(image, path.c_str());
        ror::log("screenshot: frame %llu %ux%u -> %s%s", static_cast<unsigned long long>(frame), w, h, path.c_str(),
                 ok ? "" : " (failed)");
    }
}

// sceGsPutDrawEnv 0x25b588 (libgraph): sends the packet at a0, a GIFtag and its NLOOP A+D pairs, to the GS by GIF DMA in
// normal mode with QWC = NLOOP + 1 (the original writes D2_QWC/MADR/CHCR 0x101). PS2Recomp's runtime stub applies 8
// pairs read from a0 itself: it takes the GIFtag for a pair and drops the rest. gs_swap_env 0x25b908 sends the
// double-buffer packets at +0x50 / +0x140 (15 quadwords apart) every frame: the draw environment and the frame's clear.
void ror_sceGsPutDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t tag = ror::arg(ctx, 0);
    const uint32_t qwc = (ror::rd32(rdram, tag) & 0x7fff) + 1;
    static int s_logged = 0;
    if (s_logged < 3 && ++s_logged)
        ror::log("sceGsPutDrawEnv: packet %08x, %u quadwords", tag, qwc);
    if (const uint8_t *p = ror::ptr(rdram, tag, qwc * 16))
        runtime->memory().submitGifPacket(GifPathId::Path3, p, qwc * 16);
    ror::ret(ctx, 0);
}

// sceGsExecStoreImage 0x25c1c0 (libgraph): reads the image a store-image packet describes into a1. The packet at a0 is
// what the game's own sceGsSetDefStoreImage 0x25bf00 built: VIF codes at +0x00 (MSKPATH3, FLUSHA, DIRECT 6), a GIFtag at
// +0x10 and five A+D pairs: BITBLTBUF +0x20, TRXPOS +0x30, TRXREG +0x40, FINISH +0x50, TRXDIR = 1 +0x60. PS2Recomp's
// stub reads a 12-byte struct of its own (its sceGsSetDefLoadImage stub's layout) from a0, so with the game's packet it
// read 0 x 1536 T8. Here: the GIF part goes on PATH3 (the original sends it by VIF1 DIRECT and reads the VIF1 FIFO
// back), then the local->host bytes come from the GS backend (PCSX2's GS under ROR_GS=pcsx2: renderer plan step 4).
// Caller: MagSystem::screenshot 0x11860c, the debug BMP writer, one 512 x 1 line per call.
void ror_sceGsExecStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t lp = ror::arg(ctx, 0), dst = ror::arg(ctx, 1);
    const uint8_t *p = ror::ptr(rdram, lp, 0x70);
    if (!p)
    {
        ror::ret(ctx, static_cast<uint32_t>(-1));
        return;
    }
    uint64_t bitbltbuf, trxreg;
    std::memcpy(&bitbltbuf, p + 0x20, 8);
    std::memcpy(&trxreg, p + 0x40, 8);
    const uint32_t rrw = static_cast<uint32_t>(trxreg & 0xfff), rrh = static_cast<uint32_t>((trxreg >> 32) & 0xfff);
    const uint32_t bytes = static_cast<uint32_t>(
        (static_cast<uint64_t>(rrw) * rrh * GSInternal::bitsPerPixel(static_cast<uint8_t>((bitbltbuf >> 24) & 0x3f)) + 7) / 8);
    uint8_t *out = ror::ptr(rdram, dst, bytes);
    if (!out)
    {
        ror::ret(ctx, static_cast<uint32_t>(-1));
        return;
    }
    runtime->memory().submitGifPacket(GifPathId::Path3, p + 0x10, 0x60);
    runtime->gs().consumeLocalToHostBytes(out, bytes);
    ror::ret(ctx, 0);
}

// SetGsCrt (syscall 2, wrapper 0x25f420), called only by sceGsResetGraph 0x25ae98 (sdk.md), which runs as game code:
// it fills libgraph's parameter block 0x2f3d90 that sceGsDefDispEnv 0x25b150 builds the display environment from. The
// BIOS sets the video timing besides SMODE2; PS2Recomp's handler sets SMODE2 only. Rule of Rose calls it once with
// (1, 3, 0) = interlaced, PAL, field mode (engine.md): the timing registers are what the BIOS left in the real game's
// PCSX2 GS dump of the A164 event (extracted/gsref/Rule of Rose_SLES-54218_20261027182940.gs, privileged block).
void ror_SetGsCrt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t inter = ror::arg(ctx, 0) & 1, omode = ror::arg(ctx, 1) & 0xff, ffmode = ror::arg(ctx, 2) & 1;
    GSRegisters &gs = runtime->memory().gs();
    gs.smode2 = inter | (ffmode << 1);
    const bool pal = inter == 1 && omode == 3 && ffmode == 0;
    if (pal)
    {
        gs.smode1 = 0x0000000740816504ull;
        gs.srfsh = 0x0000000000000008ull;
        gs.synch1 = 0x0007f5c21fc83030ull;
        gs.synch2 = 0x00000000003484bcull;
        gs.syncv = 0x00a9000502101401ull;
    }
    ror::log("SetGsCrt(interlace %u, mode %u, ffmode %u)%s", inter, omode, ffmode,
             pal ? ": PAL interlaced field timing" : ": no timing table for this mode, SMODE2 only");
    ror::ret(ctx, 0);
}

namespace ror
{
    void gsInstall(PS2Runtime &runtime, bool pcsx2)
    {
        g_pcsx2 = pcsx2;
        const char *f = std::getenv("ROR_GS_TRACE");
        g_traceFrame = f && *f ? std::strtoull(f, nullptr, 10) : 0;
        for (const char *p = std::getenv("ROR_SCREENSHOT"); p && *p;)
        {
            char *end = nullptr;
            const uint64_t n = std::strtoull(p, &end, 10);
            if (end == p)
                break;
            g_shotFrames.push_back(n);
            p = *end == ',' ? end + 1 : end;
        }
        std::unique_ptr<GSRasterBackend> raster;
        if (pcsx2)
            raster = std::make_unique<NullBackend>();
        else
            raster = std::make_unique<GSCpuBackend>();
        auto backend = std::make_unique<TracingBackend>(std::move(raster));
        g_backend = backend.get();
        runtime.gs().setRasterBackend(std::move(backend));
        if (pcsx2)
            runtime.setExternalPresentation([](PS2Runtime &, void *)
                                            {
                                                ror::windowSetup();
                                                ror::pcsx2WindowInit(GetWindowHandle());
                                            },
                                            [](PS2Runtime &, void *)
                                            {
                                                ror::windowLoop();
                                                ror::pcsx2WindowLoop();
                                            },
                                            [](PS2Runtime &, void *) { ror::pcsx2WindowShutdown(); }, nullptr);

        // One observer per kind (patches 0008, 0010) for the GS dumps and PCSX2's live feed. With ROR_GS=pcsx2 the VIF1
        // sink (patch 0015) gives every DMA channel 1 stream to PCSX2's VIF1 + microVU1 instead of PS2Recomp's, so the
        // GIF arbiter sees PATH3 only: GIF DMA, sceGsPutDrawEnv, load-image (renderer plan 6.5a).
        const bool dumps = ror::gsDumpInstall(runtime, pcsx2);
        if (!dumps && !pcsx2)
            return;
        runtime.gifArbiter().setObserver([](GifPathId id, const uint8_t *data, uint32_t size)
                                         {
            ror::gsDumpGif(id, data, size);
            if (!g_pcsx2)
                return;
            static bool s_warned = false;
            if (id != GifPathId::Path3 && !s_warned && (s_warned = true))
                ror::log("pcsx2: a PATH%d packet reached PS2Recomp's GIF arbiter (sent to PCSX2 as PATH3)",
                         id == GifPathId::Path1 ? 1 : 2);
            ror::pcsx2Path3(data, size); });
        // Register writes runtime handlers make outside the GIF stream: a one-register A+D packet on PATH3.
        runtime.gs().setRegisterObserver([](uint8_t reg, uint64_t value)
                                         {
            const uint64_t packet[4] = {1ull | (1ull << 15) | (1ull << 60), 0xe, value, reg}; // NLOOP 1, EOP, PACKED, A+D
            ror::gsDumpRegister(reg, value, packet);
            if (g_pcsx2)
                ror::pcsx2Path3(packet, sizeof(packet)); });
        if (dumps && ror::gsDumpVif1Wanted())
            runtime.memory().setVif1Observer([](const uint8_t *data, uint32_t size) { ror::gsDumpVif1(data, size); });
        if (pcsx2)
            runtime.memory().setVif1Sink([](const uint8_t *data, uint32_t size) { ror::pcsx2Vif1(data, size); });
    }

    void gsVsync(PS2Runtime *runtime)
    {
        if (!g_pcsx2)
            return;
        static std::vector<uint8_t> regs;
        g_field = !g_field;
        gsPrivRegs(*runtime, g_field, regs);
        ror::pcsx2Vsync(regs.data(), regs.size());
    }

    uint64_t gsBackendNanos() { return g_backendNs.load(std::memory_order_relaxed); }

    void gsSnapshotVram(std::vector<uint8_t> &out)
    {
        out.clear();
        if (!g_backend)
            return;
        g_backend->Flush();
        g_backend->Sync(GSSyncReason::DebugReadback);
        g_backend->SnapshotVram(out);
    }

    // Called by wait_vsync_swap on entry (leaving = false) and exit (leaving = true) with the frame count.
    void gsFrameMark(PS2Runtime *runtime, uint64_t frame, bool leaving)
    {
        g_gameThread = std::this_thread::get_id();
        if (leaving && std::find(g_shotFrames.begin(), g_shotFrames.end(), frame) != g_shotFrames.end())
            screenshot(runtime, frame);
        if (leaving)
            ror::gsDumpFrameMark(frame);
        // ROR_REGIONS=F[,…]: GamMan's trigger regions after frame F (the list FUN_001e02c8 walks each frame: head
        // GamMan+0x40+0xc290 = 0x7b7490, first node at head+4, next at node+4; x0 z0 x1 z1 at +0x10..+0x1c, y at
        // +0x24, flags +0x30, script function id +0x74), to ror.log.
        for (const char *p = leaving ? std::getenv("ROR_REGIONS") : nullptr; p && *p;)
        {
            char *end = nullptr;
            if (std::strtoull(p, &end, 10) == frame)
            {
                uint8_t *ram = runtime->memory().getRDRAM();
                auto f = [&](uint32_t a) { const uint32_t u = ror::rd32(ram, a); float v; std::memcpy(&v, &u, 4); return v; };
                constexpr uint32_t kHead = 0x7b7490;
                int n = 0;
                for (uint32_t node = ror::rd32(ram, kHead + 4); node != kHead && node && n < 512; node = ror::rd32(ram, node + 4), ++n)
                {
                    ror::log("region %08x: x %.0f..%.0f z %.0f..%.0f y %.0f flags %08x id %d", node, f(node + 0x10),
                             f(node + 0x18), f(node + 0x14), f(node + 0x1c), f(node + 0x24), ror::rd32(ram, node + 0x30),
                             static_cast<int16_t>(ror::rd32(ram, node + 0x74)));
                    // Object regions (flags 0x1000 near, 0x4000 facing; walk 0x1e068c): object id +0x54, its record
                    // slot +0x5c (-1 until 0x1cd0c4 finds it), radius +0x4c; the object's position at record +0x40.
                    if (ror::rd32(ram, node + 0x30) & 0x5000)
                    {
                        const int32_t slot = static_cast<int32_t>(ror::rd32(ram, node + 0x5c));
                        const uint32_t rec = 0x7285c0 + static_cast<uint32_t>(slot) * 0x4d0;
                        ror::log("  object %08x slot %d radius %.0f at %.0f %.0f %.0f", ror::rd32(ram, node + 0x54), slot,
                                 f(node + 0x4c), slot >= 0 ? f(rec + 0x40) : 0.0f, slot >= 0 ? f(rec + 0x44) : 0.0f,
                                 slot >= 0 ? f(rec + 0x48) : 0.0f);
                    }
                }
                ror::log("regions: %d", n);
            }
            if (end == p || *end != ',')
                break;
            p = end + 1;
        }
        // ROR_PEEK=F:ADDR:N[,…]: N guest words at ADDR (hex) after frame F, to ror.log.
        for (const char *p = leaving ? std::getenv("ROR_PEEK") : nullptr; p && *p;)
        {
            char *end = nullptr;
            const uint64_t f = std::strtoull(p, &end, 10);
            const uint32_t addr = *end == ':' ? static_cast<uint32_t>(std::strtoul(end + 1, &end, 16)) : 0;
            const uint32_t n = *end == ':' ? static_cast<uint32_t>(std::strtoul(end + 1, &end, 10)) : 1;
            if (f == frame)
                for (uint32_t i = 0; i < n; i += 4)
                {
                    std::string line;
                    for (uint32_t j = i; j < n && j < i + 4; ++j)
                    {
                        char w[12];
                        std::snprintf(w, sizeof(w), " %08x", ror::rd32(runtime->memory().getRDRAM(), addr + j * 4));
                        line += w;
                    }
                    ror::log("peek %08x:%s", addr + i * 4, line.c_str());
                }
            if (end == p || *end != ',')
                break;
            p = end + 1;
        }
        if (!g_traceFrame)
            return;
        if (leaving && frame == g_traceFrame && !g_out)
        {
            const char *o = std::getenv("ROR_GS_TRACE_OUT");
            const std::string path = o && *o ? o : "gs_frame_" + std::to_string(frame) + ".tsv";
            g_out = std::fopen(path.c_str(), "w");
            g_draws = 0;
            if (g_out)
            {
                ror::log("gs trace: frame %llu -> %s", static_cast<unsigned long long>(frame), path.c_str());
                std::fprintf(g_out, "# GS trace of frame %llu (end of wait_vsync_swap %llu to the start of %llu)\n",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(frame),
                             static_cast<unsigned long long>(frame + 1));
                writePrivileged(runtime);
                dumpVram("gs_frame_" + std::to_string(frame) + "_start.vram");
            }
        }
        else if (!leaving && frame == g_traceFrame && g_out)
        {
            writePrivileged(runtime);
            dumpVram("gs_frame_" + std::to_string(frame) + "_end.vram");
            std::fclose(g_out);
            g_out = nullptr;
            ror::log("gs trace: %u draws", g_draws);
        }
    }
}
