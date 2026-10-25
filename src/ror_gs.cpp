// GS diagnostics: a raster backend that wraps PS2Recomp's GSCpuBackend (installed with GS::setRasterBackend, which
// supports hot replacement), times the backend calls made on the game thread, and for one frame (ROR_GS_TRACE=N)
// writes every draw, CLUT load and transfer with its decoded GS state, plus the privileged display registers, to
// ROR_GS_TRACE_OUT (default gs_frame_N.tsv). A "frame" is the span from the end of MagSystem::wait_vsync_swap N to
// the start of N+1: kick_dma sends frame N's chain there (MagSystem::frame 0x11848c), followed by frame N+1's
// immediate texture uploads. tools/port/gs_trace.py summarises the file.
// ROR_SCREENSHOT=F[,F...] saves the presented frame (what the window shows) at the end of wait_vsync_swap F as
// screen_F.png.
#include "ror.h"

#include "runtime/gs/gs_cpu_backend.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

    TracingBackend *g_backend = nullptr;

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

    // The frame the render thread latched last (GS::latchHostPresentationFrame, once per vsync tick), opaque.
    void screenshot(PS2Runtime *runtime, uint64_t frame)
    {
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

namespace ror
{
    void gsInstall(PS2Runtime &runtime)
    {
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
        auto backend = std::make_unique<TracingBackend>(std::make_unique<GSCpuBackend>());
        g_backend = backend.get();
        runtime.gs().setRasterBackend(std::move(backend));
        ror::gsDumpInstall(runtime);
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
