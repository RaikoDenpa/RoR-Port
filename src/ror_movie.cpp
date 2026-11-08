// Movie seam (notes/port.md "Movies"): our own player in place of rxmpeg's stream reading and libmpeg's IPU decode.
// The game keeps its player setup 0x182cb4 (picture buffers, libmpeg create), reset 0x182bfc, teardown 0x182ee8, its
// player interface 0x181820-0x181e50 (TskMovie, the title's attract mode), the worker-stop 0x182ae4 and the display
// 0x181978; we replace the five functions that touch the stream:
//
//   0x18313c open, waits for start (TskMovie: 0x181c3c)   0x18301c open, plays at once (title state 0x1af6ac)
//   0x18324c start                                         0x1832d8 per-frame update   0x182b98 stream close
//
// Our update reproduces the game's update and its decode worker 0x182824 (not started: it spins without a syscall):
// one picture decoded ahead into slot 0x710278 while 0x710280 > 1, a picture consumed every other frame (0x71026c),
// so 25 pictures/s at the movie interval 1 (50 Hz). Pictures: RGBA, alpha 0x80, in the layout the display uploads,
// 16-pixel-wide columns of the movie's height (0x710204), column c at c * height * 64 bytes, slot stride
// 0x29af98 * 0x29af9c * 4 (0x183634). The game's state as its readers see it:
//   0x29af7c ready (0x1832cc), 0x29af58 finished (0x1837c0), 0x29af94 last 0x2800 bytes (0x183130, the fade cue),
//   0x29af48 bytes left, 0x710204 picture height (0x183628: 0 = nothing to draw), 0x710208 decoded pictures
//   (0x1832b8, script native 253; UNVERIFIED whether libmpeg counts from 0 or 1), 0x29af54 pause, 0x29af50 volume.
//
// Video: FFmpeg's MPEG-2 decoder (avcodec/avutil, build.py ffmpeg: gyan.dev's 8.1.2 build, GPL v3), loaded at runtime
// from ROR_FFMPEG_DIR or next to the runner; without it every movie is skipped as before. Colour: ITU-R BT.601, MPEG-2's default matrix, studio range
// (UNVERIFIED against the IPU's output: no real-game dump of a movie yet). Audio: private stream 1, sub-stream 0xff,
// a Sony SShd/SSbd PS-ADPCM stream (notes/formats/pss.md, tools/pss_audio.py), decoded here and played through the
// runtime's raylib audio device. ROR_MOVIES=0 skips all movies (the old placeholder behaviour).
#include "ror.h"
#include "ror_adpcm.h"

#include "raylib.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

extern "C" __declspec(dllimport) void *__stdcall LoadLibraryW(const wchar_t *name);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void *module, wchar_t *name,
                                                                           unsigned long size);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);

namespace
{
    // ---- the game's player state (rxmpeg globals, libmpeg's decoder struct at 0x710200) ----
    constexpr uint32_t kHandle = 0x29af44, kLeft = 0x29af48, kSize = 0x29af4c, kVolume = 0x29af50, kPause = 0x29af54,
                       kFinished = 0x29af58, kConsumed = 0x29af5c, kStarted = 0x29af78, kReady = 0x29af7c,
                       kPictures = 0x29af80, kAudioOn = 0x29af88, kNearEnd = 0x29af94, kBufW = 0x29af98,
                       kBufH = 0x29af9c, kDecodes = 0x29afa0, kFlag014 = 0x29b014;
    constexpr uint32_t kMpegW = 0x710200, kMpegH = 0x710204, kMpegFrames = 0x710208, kToggle = 0x71026c,
                       kToggle2 = 0x710270, kFlag274 = 0x710274, kWriteSlot = 0x710278, kShowSlot = 0x71027c,
                       kFree = 0x710280;
    constexpr uint32_t kWorkerStop = 0x182ae4, kAudioClose = 0x182068;

    // ---- FFmpeg, the few entry points used, loaded at runtime. Struct prefixes as in FFmpeg's public headers
    // (libavutil/frame.h AVFrame, libavcodec/packet.h AVPacket; stable since FFmpeg 4). ----
    struct AVFramePrefix
    {
        uint8_t *data[8];
        int linesize[8];
        uint8_t **extended_data;
        int width, height;
        int nb_samples;
        int format;
    };
    struct AVPacketPrefix
    {
        void *buf;
        int64_t pts, dts;
        uint8_t *data;
        int size;
    };
    constexpr int kCodecMpeg2 = 2;                 // AV_CODEC_ID_MPEG2VIDEO
    constexpr int kPixYuv420p = 0;                 // AV_PIX_FMT_YUV420P
    constexpr int kEagain = -11;                   // AVERROR(EAGAIN) with the MSVC CRT's EAGAIN 11
    constexpr int64_t kNoPts = static_cast<int64_t>(0x8000000000000000ull);
    constexpr int kAudioChunk = 4096;              // audio frames per raylib stream update

    struct Ff
    {
        bool tried = false, ok = false;
        const void *(*find_decoder)(int);
        void *(*alloc_context3)(const void *);
        int (*open2)(void *, const void *, void **);
        int (*send_packet)(void *, const void *);
        int (*receive_frame)(void *, void *);
        void (*free_context)(void **);
        void *(*packet_alloc)();
        void (*packet_free)(void **);
        void *(*parser_init)(int);
        int (*parser_parse2)(void *, void *, uint8_t **, int *, const uint8_t *, int, int64_t, int64_t, int64_t);
        void (*parser_close)(void *);
        void *(*frame_alloc)();
        void (*frame_free)(void **);
    } g_ff;

    bool loadFfmpeg()
    {
        if (g_ff.tried)
            return g_ff.ok;
        g_ff.tried = true;
        // avutil-N.dll and avcodec-N.dll (any N: FFmpeg's struct prefixes above are stable) in ROR_FFMPEG_DIR or next
        // to the runner (build.py ffmpeg, then build).
        namespace fs = std::filesystem;
        fs::path dir;
        if (const char *e = std::getenv("ROR_FFMPEG_DIR"); e && *e)
            dir = e;
        else
        {
            wchar_t exe[1024] = {};
            GetModuleFileNameW(nullptr, exe, 1024);
            dir = fs::path(exe).parent_path();
        }
        auto find = [&](const char *prefix)
        {
            std::error_code ec;
            for (const auto &f : fs::directory_iterator(dir, ec))
            {
                const std::string n = f.path().filename().string();
                if (n.rfind(prefix, 0) == 0 && f.path().extension() == ".dll")
                    return f.path();
            }
            return fs::path();
        };
        const fs::path utilPath = find("avutil-"), codecPath = find("avcodec-");
        void *util = utilPath.empty() ? nullptr : LoadLibraryW(utilPath.c_str());
        void *codec = util && !codecPath.empty() ? LoadLibraryW(codecPath.c_str()) : nullptr;
        if (!codec)
        {
            ror::log("movies: no FFmpeg (avutil-*.dll, avcodec-*.dll) in %s; movies are skipped", dir.string().c_str());
            return false;
        }
        bool all = true;
        auto get = [&](void *lib, const char *name, auto &fn)
        {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(lib, name));
            if (!fn)
            {
                ror::log("movies: FFmpeg has no %s", name);
                all = false;
            }
        };
        get(codec, "avcodec_find_decoder", g_ff.find_decoder);
        get(codec, "avcodec_alloc_context3", g_ff.alloc_context3);
        get(codec, "avcodec_open2", g_ff.open2);
        get(codec, "avcodec_send_packet", g_ff.send_packet);
        get(codec, "avcodec_receive_frame", g_ff.receive_frame);
        get(codec, "avcodec_free_context", g_ff.free_context);
        get(codec, "av_packet_alloc", g_ff.packet_alloc);
        get(codec, "av_packet_free", g_ff.packet_free);
        get(codec, "av_parser_init", g_ff.parser_init);
        get(codec, "av_parser_parse2", g_ff.parser_parse2);
        get(codec, "av_parser_close", g_ff.parser_close);
        get(util, "av_frame_alloc", g_ff.frame_alloc);
        get(util, "av_frame_free", g_ff.frame_free);
        g_ff.ok = all;
        if (all)
            ror::log("movies: FFmpeg loaded (%s, %s)", codecPath.filename().string().c_str(),
                     utilPath.filename().string().c_str());
        return all;
    }

    struct Movie
    {
        FILE *f = nullptr;
        std::string name;
        uint64_t size = 0, pos = 0;          // file bytes demuxed so far
        bool eof = false, flushed = false, drained = false;
        std::vector<uint8_t> in;             // file bytes not yet demuxed
        std::deque<uint8_t> video;           // MPEG-2 elementary stream
        void *ctx = nullptr, *parser = nullptr, *pkt = nullptr, *frm = nullptr;
        uint32_t pictures = 0;
        // audio
        std::vector<uint8_t> audioRaw;       // the SShd/SSbd stream as demuxed
        size_t audioBody = 0;                // offset of the ADPCM body in audioRaw (0 = header not parsed yet)
        int rate = 0, channels = 0, interleave = 0;
        size_t audioPos = 0;                 // next undecoded byte of the body
        ror::Adpcm dec[2];
        std::vector<int16_t> pcm;            // decoded, interleaved, not yet queued
        AudioStream stream{};
        bool streamOn = false, playing = false;
        float volume = -1.0f;
        uint32_t queued = 0, starved = 0;    // sub-buffers handed to raylib; refills missed for want of samples
        FILE *wav = nullptr;                 // ROR_MOVIE_WAV: the decoded audio, as tools/pss_audio.py --wav writes it
        uint32_t wavFrames = 0;
    };
    Movie *g_movie = nullptr;

    // MPEG-2 program stream: pack headers (0xba), system header and PES packets (0xbb..0xff, 16-bit length).
    // Video: stream 0xe0. Audio: private stream 1 (0xbd) whose payload starts with ff a1 00 00.
    bool demux(Movie &m)
    {
        if (m.eof)
            return false;
        uint8_t chunk[0x10000];
        const size_t n = std::fread(chunk, 1, sizeof chunk, m.f);
        if (n == 0)
            m.eof = true;
        m.in.insert(m.in.end(), chunk, chunk + n);
        size_t i = 0;
        const std::vector<uint8_t> &d = m.in;
        while (i + 6 <= d.size())
        {
            if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1)
            {
                ++i;
                continue;
            }
            const uint8_t sid = d[i + 3];
            if (sid == 0xba)
            {
                if (i + 14 > d.size())
                    break;
                i += 14 + (d[i + 13] & 7);
                continue;
            }
            if (sid == 0xb9)    // program end
            {
                i += 4;
                continue;
            }
            if (sid < 0xbb)
            {
                ++i;
                continue;
            }
            const size_t len = (static_cast<size_t>(d[i + 4]) << 8) | d[i + 5];
            if (i + 6 + len > d.size())
                break;
            if ((sid == 0xe0 || sid == 0xbd) && len >= 3)
            {
                const size_t hdr = 9 + d[i + 8];
                if (hdr <= 6 + len)
                {
                    const uint8_t *p = &d[i + hdr];
                    const size_t plen = 6 + len - hdr;
                    if (sid == 0xe0)
                        m.video.insert(m.video.end(), p, p + plen);
                    else if (plen > 4 && p[0] == 0xff)
                        m.audioRaw.insert(m.audioRaw.end(), p + 4, p + plen);
                }
            }
            i += 6 + len;
        }
        m.pos += i;
        m.in.erase(m.in.begin(), m.in.begin() + static_cast<ptrdiff_t>(i));
        if (m.eof)
        {
            m.pos = m.size;
            m.in.clear();
        }
        return true;
    }

    // BT.601 studio range -> RGB, 16.16 fixed point (ITU-R BT.601 / ISO 13818-2 default matrix_coefficients).
    inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

    void writePicture(uint8_t *rdram, const AVFramePrefix &fr, uint32_t slot)
    {
        const uint32_t bufW = ror::rd32(rdram, kBufW), bufH = ror::rd32(rdram, kBufH);
        const int w = std::min<int>(fr.width, static_cast<int>(bufW)) & ~15;
        const int h = std::min<int>(fr.height, static_cast<int>(bufH)) & ~15;
        const uint32_t base = ((ror::rd32(rdram, kPictures) + 0x3f) & ~0x3fu) + slot * bufW * bufH * 4;
        uint8_t *dst = ror::ptr(rdram, base, bufW * bufH * 4);
        if (!dst || w <= 0 || h <= 0)
            return;
        for (int y = 0; y < h; ++y)
        {
            const uint8_t *ly = fr.data[0] + static_cast<ptrdiff_t>(y) * fr.linesize[0];
            const uint8_t *lu = fr.data[1] + static_cast<ptrdiff_t>(y / 2) * fr.linesize[1];
            const uint8_t *lv = fr.data[2] + static_cast<ptrdiff_t>(y / 2) * fr.linesize[2];
            for (int x = 0; x < w; ++x)
            {
                const int c = (ly[x] - 16) * 76309, u = lu[x / 2] - 128, v = lv[x / 2] - 128;
                uint8_t *px = dst + static_cast<size_t>(x >> 4) * h * 64 + static_cast<size_t>(y) * 64 + (x & 15) * 4;
                px[0] = clamp8((c + 104597 * v + 32768) >> 16);
                px[1] = clamp8((c - 25675 * u - 53279 * v + 32768) >> 16);
                px[2] = clamp8((c + 132201 * u + 32768) >> 16);
                px[3] = 0x80;
            }
        }
        ror::wr32(rdram, kMpegW, static_cast<uint32_t>(w));
        ror::wr32(rdram, kMpegH, static_cast<uint32_t>(h));
    }

    // Next picture into `slot`; false at the end of the stream.
    bool decodePicture(uint8_t *rdram, Movie &m, uint32_t slot)
    {
        auto *pkt = static_cast<AVPacketPrefix *>(m.pkt);
        for (;;)
        {
            const int r = g_ff.receive_frame(m.ctx, m.frm);
            if (r == 0)
            {
                const auto &fr = *static_cast<AVFramePrefix *>(m.frm);
                if (fr.format != kPixYuv420p)
                {
                    ror::log("movies: %s: pixel format %d, not 4:2:0", m.name.c_str(), fr.format);
                    return false;
                }
                writePicture(rdram, fr, slot);
                ++m.pictures;
                return true;
            }
            if (r != kEagain || m.drained)
                return false;
            // Feed the parser: whole pictures out of the elementary stream.
            uint8_t *out = nullptr;
            int outSize = 0;
            if (m.video.size() < 0x10000 && !m.eof)
            {
                demux(m);
                continue;
            }
            if (m.video.empty() && m.eof)
            {
                if (!m.flushed)
                {
                    g_ff.parser_parse2(m.parser, m.ctx, &out, &outSize, nullptr, 0, kNoPts, kNoPts, 0);
                    m.flushed = true;
                    if (outSize > 0)
                    {
                        pkt->data = out;
                        pkt->size = outSize;
                        g_ff.send_packet(m.ctx, m.pkt);
                    }
                    continue;
                }
                g_ff.send_packet(m.ctx, nullptr);   // drain the decoder's last pictures
                m.drained = true;
                continue;
            }
            std::vector<uint8_t> chunk(m.video.begin(), m.video.begin() + std::min<size_t>(m.video.size(), 0x10000));
            const int used = g_ff.parser_parse2(m.parser, m.ctx, &out, &outSize, chunk.data(),
                                                static_cast<int>(chunk.size()), kNoPts, kNoPts, 0);
            if (used < 0)
                return false;
            m.video.erase(m.video.begin(), m.video.begin() + used);
            if (outSize > 0)
            {
                pkt->data = out;
                pkt->size = outSize;
                pkt->pts = pkt->dts = kNoPts;
                if (g_ff.send_packet(m.ctx, m.pkt) < 0)
                    ror::log("movies: %s: decoder rejected a %d-byte picture", m.name.c_str(), outSize);
            }
        }
    }

    // ---- audio: SShd header, then the PS-ADPCM body, channels interleaved every `interleave` bytes ----
    void wavHeader(FILE *f, int rate, int channels, uint32_t frames)
    {
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        const uint32_t data = frames * static_cast<uint32_t>(channels) * 2;
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f);
        u32(36 + data);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16);
        u16(1);
        u16(static_cast<uint16_t>(channels));
        u32(static_cast<uint32_t>(rate));
        u32(static_cast<uint32_t>(rate * channels * 2));
        u16(static_cast<uint16_t>(channels * 2));
        u16(16);
        std::fwrite("data", 1, 4, f);
        u32(data);
    }

    void audioParseHeader(Movie &m)
    {
        if (m.audioBody || m.audioRaw.size() < 0x28)
            return;
        const uint8_t *a = m.audioRaw.data();
        uint32_t w[8];
        std::memcpy(w, a, sizeof w);
        const uint32_t hsize = w[1];
        if (std::memcmp(a, "SShd", 4) != 0 || m.audioRaw.size() < 8 + hsize + 8 ||
            std::memcmp(a + 8 + hsize, "SSbd", 4) != 0 || w[2] != 0x10 || (w[4] != 1 && w[4] != 2) || !w[5])
        {
            ror::log("movies: %s: audio is not a PS-ADPCM SShd/SSbd stream; silent", m.name.c_str());
            m.audioBody = SIZE_MAX;
            return;
        }
        m.rate = static_cast<int>(w[3]);
        m.channels = static_cast<int>(w[4]);
        m.interleave = static_cast<int>(w[5]);
        m.audioBody = 8 + hsize + 8;
        m.audioPos = m.audioBody;
        // Sub-buffers of kAudioChunk frames (raylib 5.5's default is the device rate / 30): set only for this stream.
        SetAudioStreamBufferSizeDefault(kAudioChunk);
        m.stream = LoadAudioStream(static_cast<unsigned>(m.rate), 16, static_cast<unsigned>(m.channels));
        SetAudioStreamBufferSizeDefault(0);
        m.streamOn = IsAudioStreamValid(m.stream);
        ror::log("movies: %s: audio %d Hz, %d channel(s), interleave %#x%s", m.name.c_str(), m.rate, m.channels,
                 m.interleave, m.streamOn ? "" : " (no audio device: silent)");
        if (const char *dir = std::getenv("ROR_MOVIE_WAV"); dir && *dir)
        {
            const std::string path = std::string(dir) + "/" + m.name.substr(0, m.name.rfind('.')) + ".wav";
            m.wav = std::fopen(path.c_str(), "wb");
            if (m.wav)
                wavHeader(m.wav, m.rate, m.channels, 0);
        }
    }

    void audioDecode(Movie &m)
    {
        if (!m.audioBody || m.audioBody == SIZE_MAX)
            return;
        // One interleave block per channel at a time: block k belongs to channel k % channels.
        const size_t group = static_cast<size_t>(m.interleave) * m.channels;
        while (m.audioPos + group <= m.audioRaw.size())
        {
            const size_t frames = static_cast<size_t>(m.interleave) / 16;
            const size_t start = m.pcm.size();
            m.pcm.resize(start + frames * 28 * m.channels);
            for (int ch = 0; ch < m.channels; ++ch)
                for (size_t k = 0; k < frames; ++k)
                    m.dec[ch].frame(&m.audioRaw[m.audioPos + ch * m.interleave + k * 16],
                                    &m.pcm[start + k * 28 * m.channels + ch], m.channels);
            m.audioPos += group;
            if (m.wav)
            {
                std::fwrite(&m.pcm[start], 2, m.pcm.size() - start, m.wav);
                m.wavFrames += static_cast<uint32_t>(frames * 28);
            }
        }
        if (m.audioPos > 0x100000)   // keep the demuxed buffer small
        {
            m.audioRaw.erase(m.audioRaw.begin(), m.audioRaw.begin() + static_cast<ptrdiff_t>(m.audioPos));
            m.audioPos = 0;
            m.audioBody = 1;   // parsed; positions are now relative
        }
    }

    void audioPump(uint8_t *rdram, Movie &m, bool run)
    {
        if (!m.streamOn)
            return;
        const float vol = static_cast<float>(std::min<uint32_t>(ror::rd32(rdram, kVolume), 0x80)) / 128.0f;
        if (vol != m.volume)
        {
            ror::log("movies: %s: volume %#x", m.name.c_str(), ror::rd32(rdram, kVolume));
            SetAudioStreamVolume(m.stream, vol);
            m.volume = vol;
        }
        if (!run)
        {
            if (m.playing)
                PauseAudioStream(m.stream);
            m.playing = false;
            return;
        }
        // raylib asks for one sub-buffer (kAudioChunk frames) at a time.
        const size_t want = kAudioChunk * static_cast<size_t>(m.channels);
        while (IsAudioStreamProcessed(m.stream) && m.pcm.size() >= want)
        {
            UpdateAudioStream(m.stream, m.pcm.data(), kAudioChunk);
            m.pcm.erase(m.pcm.begin(), m.pcm.begin() + static_cast<ptrdiff_t>(want));
            ++m.queued;
        }
        if (m.playing && IsAudioStreamProcessed(m.stream) && !m.eof)
            ++m.starved;
        if (!m.playing)
        {
            if (IsAudioStreamPlaying(m.stream))
                ResumeAudioStream(m.stream);
            else
                PlayAudioStream(m.stream);
            m.playing = true;
        }
    }

    void closeMovie()
    {
        if (!g_movie)
            return;
        Movie &m = *g_movie;
        if (m.streamOn)
        {
            StopAudioStream(m.stream);
            UnloadAudioStream(m.stream);
        }
        if (m.wav)
        {
            wavHeader(m.wav, m.rate, m.channels, m.wavFrames);
            std::fclose(m.wav);
        }
        if (m.rate)
            ror::log("movies: %s: audio %u sub-buffers of %d frames queued, %u frames found one waiting unfilled",
                     m.name.c_str(), m.queued, kAudioChunk, m.starved);
        if (m.frm)
            g_ff.frame_free(&m.frm);
        if (m.pkt)
            g_ff.packet_free(&m.pkt);
        if (m.parser)
            g_ff.parser_close(m.parser);
        if (m.ctx)
            g_ff.free_context(&m.ctx);
        if (m.f)
            std::fclose(m.f);
        ror::log("movies: %s closed after %u pictures", m.name.c_str(), m.pictures);
        delete g_movie;
        g_movie = nullptr;
    }

    bool moviesEnabled()
    {
        static const bool on = []
        {
            const char *e = std::getenv("ROR_MOVIES");
            return !(e && *e == '0');
        }();
        return on;
    }

    // The old placeholder's state: the end of a stream (prebuffered, past the last 0x2800 bytes, finished), handle
    // -1 so start and update do nothing and no worker runs.
    void skip(uint8_t *rdram, R5900Context *ctx, const std::string &name, const char *why)
    {
        ror::log("movie movie/%s skipped (%s)", name.c_str(), why);
        ror::wr32(rdram, kHandle, 0xffffffffu);
        ror::wr32(rdram, kLeft, 0);
        ror::wr32(rdram, kFinished, 1);
        ror::wr32(rdram, kReady, 1);
        ror::wr32(rdram, kNearEnd, 1);
        ror::ret(ctx, 0);
    }

    void open(uint8_t *rdram, R5900Context *ctx, bool play)
    {
        const std::string name = ror::str(rdram, ror::arg(ctx, 0), 0x40);
        closeMovie();
        if (!moviesEnabled())
            return skip(rdram, ctx, name, "ROR_MOVIES=0");
        if (!loadFfmpeg())
            return skip(rdram, ctx, name, "no FFmpeg");
        const std::string host = ror::discFile("movie/" + name);
        FILE *f = host.empty() ? nullptr : std::fopen(host.c_str(), "rb");
        if (!f)
            return skip(rdram, ctx, name, "not on the disc");
        auto *m = new Movie;
        m->f = f;
        m->name = name;
        std::fseek(f, 0, SEEK_END);
        m->size = static_cast<uint64_t>(std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        const void *codec = g_ff.find_decoder(kCodecMpeg2);
        m->ctx = codec ? g_ff.alloc_context3(codec) : nullptr;
        m->parser = g_ff.parser_init(kCodecMpeg2);
        m->pkt = g_ff.packet_alloc();
        m->frm = g_ff.frame_alloc();
        g_movie = m;
        if (!m->ctx || !m->parser || !m->pkt || !m->frm || g_ff.open2(m->ctx, codec, nullptr) < 0)
        {
            closeMovie();
            return skip(rdram, ctx, name, "FFmpeg's MPEG-2 decoder did not open");
        }
        // As the game's opens: handle (any value >= 0), bytes left, flags; libmpeg's picture height 0 until the first
        // picture (the display draws nothing before).
        const uint32_t size = static_cast<uint32_t>(std::min<uint64_t>(m->size, 0x7fffffff));
        ror::wr32(rdram, kNearEnd, 0);
        ror::wr32(rdram, kDecodes, 0);
        ror::wr32(rdram, kHandle, 1);
        ror::wr32(rdram, kLeft, size);
        ror::wr32(rdram, kSize, size);
        ror::wr32(rdram, kFinished, 0);
        ror::wr32(rdram, kConsumed, 0);
        ror::wr32(rdram, kAudioOn, 0);
        ror::wr32(rdram, kStarted, play ? 1 : 0);
        ror::wr32(rdram, kMpegH, 0);
        ror::wr32(rdram, kMpegFrames, 0);
        ror::log("movie movie/%s: %llu bytes, %s", name.c_str(), static_cast<unsigned long long>(m->size),
                 play ? "playing" : "waiting for start");
        ror::ret(ctx, 1);
    }
}

void ror_movie_open(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { open(rdram, ctx, false); }
void ror_movie_open_play(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { open(rdram, ctx, true); }

void ror_movie_start(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    if (static_cast<int32_t>(ror::rd32(rdram, kHandle)) >= 0)
        ror::wr32(rdram, kStarted, 1);
    ror::ret(ctx, 0);
}

void ror_movie_update(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    ror::wr32(rdram, kFlag014, 0);   // FUN_00182818(0)
    Movie *m = g_movie;
    if (!m || static_cast<int32_t>(ror::rd32(rdram, kHandle)) < 0)
        return ror::ret(ctx, 0);
    ror::wr32(rdram, kReady, 1);   // the game's: once 0x30000 bytes are buffered; ours reads on demand
    if (ror::rd32(rdram, kStarted) != 1)
    {
        audioPump(rdram, *m, false);
        return ror::ret(ctx, 0);
    }
    // The decode worker: one picture ahead into the free slot.
    while (static_cast<int32_t>(ror::rd32(rdram, kFree)) > 1)
    {
        const uint32_t slot = ror::rd32(rdram, kWriteSlot);
        if (!decodePicture(rdram, *m, slot))
            break;
        ror::wr32(rdram, kFree, ror::rd32(rdram, kFree) - 1);
        ror::wr32(rdram, kWriteSlot, (slot + 1) % 2);
        ror::wr32(rdram, kDecodes, ror::rd32(rdram, kDecodes) + 1);
        ror::wr32(rdram, kMpegFrames, m->pictures);
    }
    audioParseHeader(*m);
    audioDecode(*m);
    // Audio is demuxed as the video needs data; keep it 3 sub-buffers ahead of raylib (the video stays queued).
    while (m->streamOn && !m->eof && m->pcm.size() < 3u * kAudioChunk * m->channels && m->video.size() < 0x400000)
    {
        demux(*m);
        audioDecode(*m);
    }
    // Bytes left: the game counts the file down as it demuxes; at the stream's end nothing is left.
    const bool done = m->drained || (m->eof && m->video.empty() && m->flushed);
    const uint32_t left = done ? 0 : static_cast<uint32_t>(std::min<uint64_t>(m->size - std::min(m->pos, m->size), 0x7fffffff));
    ror::wr32(rdram, kLeft, left);
    if (ror::rd32(rdram, kAudioOn) == 0 && static_cast<int32_t>(ror::rd32(rdram, kFree)) < 2)
        ror::wr32(rdram, kAudioOn, 1);   // the game starts its audio with the first picture (0x181ff0)
    if (ror::rd32(rdram, kAudioOn) != 0)
    {
        if (static_cast<int32_t>(left) < 0x2800)
            ror::wr32(rdram, kNearEnd, 1);
        const bool paused = ror::rd32(rdram, kPause) != 0;
        if (ror::rd32(rdram, kToggle) == 0 && !paused)
        {
            ror::wr32(rdram, kConsumed, ror::rd32(rdram, kConsumed) + 1);
            if (static_cast<int32_t>(ror::rd32(rdram, kFree)) < 2)
            {
                ror::wr32(rdram, kFree, ror::rd32(rdram, kFree) + 1);
                ror::wr32(rdram, kFlag274, 1);
                ror::wr32(rdram, kToggle2, 0);
                ror::wr32(rdram, kShowSlot, (ror::rd32(rdram, kShowSlot) + 3) % 2);
            }
            else
                ror::wr32(rdram, kFinished, ror::rd32(rdram, kFinished) | (static_cast<int32_t>(left) < 5 ? 1u : 0u));
        }
        ror::wr32(rdram, kToggle, ror::rd32(rdram, kToggle) ^ 1);
        ror::wr32(rdram, kToggle2, ror::rd32(rdram, kToggle2) ^ 1);
        audioPump(rdram, *m, !paused);
    }
    ror::ret(ctx, 0);
}

void ror_movie_close(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // The game's 0x182b98: bytes left 0, stop the decode worker (none runs: 0x182ae4 just checks), close the stream,
    // close its audio channel (0x182068).
    closeMovie();
    ror::wr32(rdram, kLeft, 0);
    ror::call(rdram, ctx, runtime, kWorkerStop, {});
    ror::wr32(rdram, kHandle, 0xffffffffu);
    ror::call(rdram, ctx, runtime, kAudioClose, {0});
    ror::ret(ctx, 0);
}
