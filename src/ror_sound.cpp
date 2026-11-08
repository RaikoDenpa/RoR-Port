// Sound seam (notes/roadmap.md seam 2): the RX sound driver's EE client (rxsnd, 0x239f60-0x2442xx; notes/sdk.md), whose
// calls went over SIF RPC to the IOP driver RXSNDDRV.IRX, is replaced by a host driver. The game keeps its own sound
// layers (GamSndMan 0x7c3fc0, sndman 0x7dc480: tables, 3D volumes, stream file reading through ror_file.cpp's ring);
// these overrides are the calls they make:
//
//   banks   0x23c0e0 rxSndXvabOpen(id or -1, EE address of the XVAB file, ...) -> id   0x23c648 loaded   0x23c4d0 close
//   SEs     0x23dd90 rxSndSePlay(bank, group, number, vol L, vol R, flag) -> handle 0..0x3f   0x23e150 status: 1
//           sounding, 5 finished (sndman's tick 0x181614 then stops it, or plays a looping entry again), 8 paused;
//           0 once stopped   0x23e020 stop   0x23e188 pause (handle, on, -)   0x23e2e8 volume (L, R)   0x23e438 pitch
//           (f12 = ratio, a0 = handle)
//   streams 0x23fc30 / 0x23fbf0 rxSndAdpcmOpen(mode 0 mono / 1, 2 stereo, rate, ...) -> handle 0..0x2f
//           0x240080 status (1 in use, 4 finished: sndman 0x180900 then closes it, 8 paused)   0x240168 free space in
//           bytes   0x240a38 / 0x240180 data (handle, EE address, -, bytes)   0x240a50 end of data   0x23ff48 close
//           0x240e20 pause   0x240f60 volume (L, R)   0x241238 pitch (f12, a0)
//   volumes 0x23a0b0 master   0x23dc98 SEs   0x23faf8 streams: each -128..128 (0x80 = unity); a voice's level is
//           master x category x its own >> 14 (0x23e2e8, 0x240f60)
//
// Formats: XVAB banks (notes/formats/xvab.md: programs, splits, SE bytecode, the libsd pitch tables, the volume
// formula of the IRX's 0x93c0), STR streams (notes/formats/str.md: headerless PS-ADPCM at 48 kHz, stereo interleaved
// every 16-byte frame). Mixing is in software at 48 kHz, kVblankFrames per VBlank of the game's clock (soundVblank,
// from ror_vsync.cpp), so what the game reads back (a stream finished, an SE still sounding) follows the game's time,
// not the audio device's; a small ring hands the mix to the device (raylib audio stream, float stereo).
//
// Diagnostics: ROR_SOUND_WAV=FILE writes the mix (the game's clock, 48 kHz stereo 16-bit); ROR_SOUND_LOG=1 logs every
// SE play (banks and streams are always logged as they open and close).
//
// ROR_SOUND=0 restores the placeholders' answers (every stream finished at once, no SE sounding, no data taken): the
// recorded routes and the regression run that way (their frames assume it, as with ROR_MOVIES=0).
#include "ror.h"
#include "ror_adpcm.h"

#include "raylib.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr int kRate = 48000;
    constexpr int kVblankFrames = kRate / 50;   // the VBlank clock is 50 Hz (PAL; ror_vsync.cpp)
    constexpr int kHandles = 0x40, kStreams = 0x30, kVoices = 96, kBanks = 0x100;
    constexpr int kReleaseFrames = 480;         // 10 ms fade when a voice is stopped
    constexpr size_t kStreamQueue = kRate / 2;  // decoded frames a stream holds ahead (its free space follows)

    bool enabled()
    {
        static const bool on = []
        {
            const char *e = std::getenv("ROR_SOUND");
            return !(e && *e == '0');
        }();
        return on;
    }

    int clampVol(int32_t v) { return v < -0x80 ? -0x80 : v > 0x80 ? 0x80 : v; }

    // ---- volumes ----
    int g_master[2] = {0x80, 0x80}, g_seVol[2] = {0x80, 0x80}, g_streamVol[2] = {0x80, 0x80};

    // ---- banks ----
    struct Sample
    {
        std::vector<int16_t> pcm;
        int64_t loop = -1;   // sample index to repeat from, -1 = one-shot
    };

    struct Bank
    {
        std::vector<uint8_t> d;
        uint32_t head = 0, body = 0;
        std::map<uint32_t, std::shared_ptr<Sample>> samples;

        uint8_t u8(uint32_t o) const { return o < d.size() ? d[o] : 0; }
        uint16_t u16(uint32_t o) const { return static_cast<uint16_t>(u8(o) | u8(o + 1) << 8); }
        uint32_t u32(uint32_t o) const { return u16(o) | static_cast<uint32_t>(u16(o + 2)) << 16; }
        // HEAD payload: 0x100, body size, then the offsets of TGIF, SEIF, VELT, PANT, LFOT
        uint32_t field(int i) const { return u32(head + 4 * i); }

        // body + offset up to the frame with the end flag (bit 0); it repeats from the frame with bit 2 when the end
        // frame has bit 1 (the SPU's loop flags)
        std::shared_ptr<Sample> sample(uint32_t off)
        {
            if (auto it = samples.find(off); it != samples.end())
                return it->second;
            auto s = std::make_shared<Sample>();
            ror::Adpcm dec;
            bool repeat = false;
            int64_t loop = -1;
            for (uint32_t p = body + off; p + 16 <= d.size() && s->pcm.size() < (1u << 24); p += 16)
            {
                const uint8_t *fr = &d[p];
                if ((fr[1] & 4) && loop < 0)
                    loop = static_cast<int64_t>(s->pcm.size());
                const size_t at = s->pcm.size();
                s->pcm.resize(at + 28);
                dec.frame(fr, &s->pcm[at], 1);
                if (fr[1] & 1)
                {
                    repeat = (fr[1] & 2) != 0;
                    break;
                }
            }
            s->loop = repeat && loop >= 0 && loop < static_cast<int64_t>(s->pcm.size()) ? loop : -1;
            samples[off] = s;
            return s;
        }
    };
    std::unique_ptr<Bank> g_banks[kBanks];

    // libsd sceSdNote2Pitch (LIBSD.IRX export 13, 0x3bd0): two u16 tables 0x8000 * 2^(n/12), 0x8000 * 2^(f/1536)
    // (tools/xvab.py note2pitch, checked entry by entry against the IRX)
    uint32_t note2pitch(int centerNote, int centerFine, int note)
    {
        static uint32_t semi[12], fine[128];
        static const bool init = []
        {
            for (int n = 0; n < 12; ++n)
                semi[n] = static_cast<uint32_t>(0x8000 * std::pow(2.0, n / 12.0));
            for (int f = 0; f < 128; ++f)
                fine[f] = static_cast<uint32_t>(0x8000 * std::pow(2.0, f / 1536.0));
            return true;
        }();
        (void)init;
        int f = centerFine;
        if (f < 0)
            return static_cast<uint32_t>(std::lround(0x1000 * std::pow(2.0, (note - centerNote) / 12.0 + f / 1536.0))) &
                   0xffff;
        const int n = note + (f >> 7) - centerNote;
        f &= 0x7f;
        const int octave = n / 12;
        int step = n - 12 * octave, shift = octave - 2;
        if (step < 0)
        {
            step += 12;
            shift -= 1;
        }
        uint32_t v = semi[step] * fine[f] >> 16;
        if (shift < 0)
            v = (v + (1u << (-shift - 1))) >> -shift;
        return v & 0xffff;
    }

    // ---- SE voices ----
    struct Voice
    {
        bool on = false;
        int handle = -1, bank = -1;
        uint32_t tag = 0;
        uint64_t serial = 0;
        std::shared_ptr<Sample> s;
        double pos = 0, step = 1;
        float gl = 0, gr = 0;   // split volume x velocity curve x pan table
        int release = -1;       // frames left of the fade, -1 = not stopping
    };
    Voice g_voices[kVoices];
    uint64_t g_serial = 0;

    struct Handle
    {
        int volL = 0x80, volR = 0x80;
        float pitch = 1.0f;
        bool alloc = false, paused = false;   // alloc: from the play to the stop
    };
    Handle g_handles[kHandles];
    int g_nextHandle = 0;

    bool handleSounding(int h)
    {
        for (const Voice &v : g_voices)
            if (v.on && v.handle == h)
                return true;
        return false;
    }

    void stopVoices(int handle, int bank, int64_t tag)
    {
        for (Voice &v : g_voices)
            if (v.on && (handle < 0 || v.handle == handle) && (bank < 0 || v.bank == bank) &&
                (tag < 0 || v.tag == static_cast<uint32_t>(tag)) && v.release < 0)
                v.release = kReleaseFrames;
    }

    Voice &freeVoice()
    {
        Voice *oldest = &g_voices[0];
        for (Voice &v : g_voices)
        {
            if (!v.on)
                return v;
            if (v.serial < oldest->serial)
                oldest = &v;
        }
        return *oldest;
    }

    // Note-on (IRX 0x8a5c -> 0x8f10): every split of the program whose key range holds the key sounds. Rate from the
    // split's root (or the key itself with flag 0x40), base pitch 0x1000 = 48 kHz; level (IRX 0x93c0) = split volume
    // x VELT[velocity] x PANT[centre], each /128.
    void noteOn(Bank &b, int bankId, int handle, uint32_t tag, int prog, int key, int vel)
    {
        const uint32_t tgif = b.field(2);
        if (!tgif || prog >= b.u16(tgif))
            return;
        const uint32_t pg = b.u32(tgif + 4 + 4 * prog);
        if (!pg)
            return;
        const int count = b.u16(pg + 2);
        const uint32_t first = b.u32(pg + 4);
        const uint32_t velt = b.field(4), pant = b.field(5);
        for (int k = 0; k < count && k < 64; ++k)
        {
            const uint32_t s = first + 0x24 * k;
            if (key < b.u8(s + 4) || key > b.u8(s + 5))
                continue;
            const uint8_t flags = b.u8(s);
            const int root = flags & 0x40 ? key : b.u8(s + 6), rootFine = flags & 0x40 ? 0 : b.u8(s + 7);
            const uint32_t pitch = std::min<uint32_t>(note2pitch(root, rootFine, key) * b.u16(s + 0xc) >> 12, 0x3fff);
            const int level = velt ? b.u8(velt + 4 + 128 * b.u8(s + 8) + std::min(vel, 127)) : vel;
            const uint32_t pt = pant ? pant + 4 + 256 * b.u8(s + 9) : 0;
            const int panL = pt ? b.u8(pt + 0x40) : 0x80, panR = pt ? b.u8(pt + 0xc0) : 0x80;
            Voice &v = freeVoice();
            v = Voice{};
            v.on = true;
            v.handle = handle;
            v.bank = bankId;
            v.tag = tag;
            v.serial = ++g_serial;
            v.s = b.sample(b.u32(s + 0x14));
            v.step = pitch / 4096.0;   // the SPU's pitch: 0x1000 = 48 kHz, the mix rate
            v.gl = b.u8(s + 0xa) / 128.0f * level / 128.0f * panL / 128.0f;
            v.gr = b.u8(s + 0xb) / 128.0f * level / 128.0f * panR / 128.0f;
            if (v.s->pcm.empty())
                v.on = false;
        }
    }

    // SE bytecode (SESQ, IRX interpreter 0x7260; tools/xvab.py sequence): op = byte & 0x3f, bit 6 = every SE; operands;
    // a varint delta time. In the data the commands that sound are note-ons at time 0, after note-offs.
    void runSe(Bank &b, int bankId, int handle, int group, int number)
    {
        const uint32_t seif = b.field(3);
        if (!seif || group >= b.u16(seif))
            return;
        const uint32_t go = b.u32(seif + 4 + 4 * group);
        if (!go || number >= b.u16(go))
            return;
        const uint32_t rq = b.u32(go + 4 + 4 * number);
        if (!rq)
            return;
        uint32_t p = b.u32(rq + 4);
        auto varint = [&]
        {
            uint32_t v = 0;
            for (int i = 0; i < 5 && p < b.d.size(); ++i)
            {
                const uint8_t x = b.d[p++];
                v = v << 7 | (x & 0x7f);
                if (!(x & 0x80))
                    break;
            }
            return v;
        };
        for (int n = 0; n < 256 && p < b.d.size(); ++n)
        {
            const uint8_t c = b.d[p++];
            const int op = c & 0x3f;
            const bool every = (c & 0x40) != 0;
            if (op == 0)
                return;
            switch (op)
            {
            case 1:
            case 3:
            case 0xb:
                break;
            case 2:
                ++p;
                break;
            case 4:
            {
                const uint32_t tag = varint();
                varint();
                const int prog = b.u8(p), key = b.u8(p + 1), vel = b.u8(p + 2);
                p += 3;
                noteOn(b, bankId, handle, tag, prog, key, vel);
                break;
            }
            case 5:
            case 6:
                stopVoices(every ? -1 : handle, every ? -1 : bankId, varint());
                break;
            case 7:
            case 9:
                varint();
                p += 2;
                break;
            case 8:
            case 10:
                varint();
                p += 2;
                varint();
                break;
            default:
                return;
            }
            varint();   // delta time
        }
    }

    // ---- ADPCM streams ----
    struct Stream
    {
        bool used = false, ended = false, paused = false;
        int channels = 1;
        ror::Adpcm dec[2];
        std::deque<int16_t> pcm[2];
        std::vector<uint8_t> partial;   // bytes short of a whole frame per channel
        int volL = 0x80, volR = 0x80;
        float pitch = 1.0f;
        double frac = 0;
    };
    Stream g_streams[kStreams];

    Stream *stream(uint32_t h) { return h < kStreams && g_streams[h].used ? &g_streams[h] : nullptr; }

    // ---- output to the audio device ----
    std::mutex g_outMutex;
    constexpr size_t kRingFrames = 16384, kMaxQueued = 7200;   // at most 150 ms queued for the device
    std::vector<float> g_ring(kRingFrames * 2);
    size_t g_ringRead = 0, g_ringFill = 0;
    AudioStream g_device{};
    bool g_deviceOn = false, g_deviceTried = false;

    void deviceCallback(void *buffer, unsigned int frames)
    {
        float *out = static_cast<float *>(buffer);
        std::lock_guard<std::mutex> lock(g_outMutex);
        const size_t n = std::min<size_t>(frames, g_ringFill);
        for (size_t i = 0; i < n; ++i)
        {
            const size_t r = (g_ringRead + i) % kRingFrames;
            out[2 * i] = g_ring[2 * r];
            out[2 * i + 1] = g_ring[2 * r + 1];
        }
        std::memset(out + 2 * n, 0, (frames - n) * 2 * sizeof(float));
        g_ringRead = (g_ringRead + n) % kRingFrames;
        g_ringFill -= n;
    }

    void push(const float *mix, size_t frames)
    {
        std::lock_guard<std::mutex> lock(g_outMutex);
        if (g_ringFill + frames > kMaxQueued)   // the game ran ahead of the device (catch-up, fast-forward): drop the oldest
        {
            const size_t drop = g_ringFill + frames - kMaxQueued;
            g_ringRead = (g_ringRead + drop) % kRingFrames;
            g_ringFill -= drop;
        }
        for (size_t i = 0; i < frames; ++i)
        {
            const size_t w = (g_ringRead + g_ringFill + i) % kRingFrames;
            g_ring[2 * w] = mix[2 * i];
            g_ring[2 * w + 1] = mix[2 * i + 1];
        }
        g_ringFill += frames;
    }

    void openDevice()
    {
        g_deviceTried = true;
        if (!IsAudioDeviceReady())
        {
            ror::log("sound: no audio device; the game's sound is mixed but not heard");
            return;
        }
        SetAudioStreamBufferSizeDefault(1024);
        g_device = LoadAudioStream(kRate, 32, 2);
        SetAudioStreamBufferSizeDefault(0);
        if (!IsAudioStreamValid(g_device))
        {
            ror::log("sound: could not open an audio stream");
            return;
        }
        SetAudioStreamCallback(g_device, deviceCallback);
        PlayAudioStream(g_device);
        g_deviceOn = true;
        ror::log("sound: host driver on (48 kHz stereo; ROR_SOUND=0 turns it off)");
    }

    FILE *g_wav = nullptr;
    uint32_t g_wavFrames = 0;
    bool g_seLog = false;

    void wavHeader()
    {
        const uint32_t data = g_wavFrames * 4;
        const uint32_t riff = 36 + data, fmtLen = 16, rate = kRate, bps = kRate * 4;
        const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
        std::fseek(g_wav, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, g_wav);
        std::fwrite(&riff, 4, 1, g_wav);
        std::fwrite("WAVEfmt ", 1, 8, g_wav);
        std::fwrite(&fmtLen, 4, 1, g_wav);
        std::fwrite(&pcm, 2, 1, g_wav);
        std::fwrite(&ch, 2, 1, g_wav);
        std::fwrite(&rate, 4, 1, g_wav);
        std::fwrite(&bps, 4, 1, g_wav);
        std::fwrite(&align, 2, 1, g_wav);
        std::fwrite(&bits, 2, 1, g_wav);
        std::fwrite("data", 1, 4, g_wav);
        std::fwrite(&data, 4, 1, g_wav);
        std::fseek(g_wav, 0, SEEK_END);
    }

    // One VBlank of sound.
    void mixVblank()
    {
        std::array<float, 2 * kVblankFrames> mix{};
        const float mL = g_master[0] / 128.0f, mR = g_master[1] / 128.0f;
        for (Stream &s : g_streams)
        {
            if (!s.used || s.paused || s.pcm[0].empty())
                continue;
            const float gl = s.volL / 128.0f * g_streamVol[0] / 128.0f * mL / 32768.0f;
            const float gr = s.volR / 128.0f * g_streamVol[1] / 128.0f * mR / 32768.0f;
            std::deque<int16_t> &l = s.pcm[0], &r = s.pcm[s.channels > 1 ? 1 : 0];
            if (s.pitch == 1.0f)
            {
                const size_t n = std::min<size_t>(kVblankFrames, std::min(l.size(), r.size()));
                for (size_t i = 0; i < n; ++i)
                {
                    mix[2 * i] += l[i] * gl;
                    mix[2 * i + 1] += r[i] * gr;
                }
                l.erase(l.begin(), l.begin() + static_cast<ptrdiff_t>(n));
                if (&r != &l)
                    r.erase(r.begin(), r.begin() + static_cast<ptrdiff_t>(n));
                continue;
            }
            const double step = std::max(0.01f, s.pitch);
            size_t i = 0;
            for (; i < kVblankFrames; ++i)
            {
                const size_t k = static_cast<size_t>(s.frac);
                if (k + 1 >= std::min(l.size(), r.size()))
                    break;
                const float f = static_cast<float>(s.frac - k);
                mix[2 * i] += (l[k] + (l[k + 1] - l[k]) * f) * gl;
                mix[2 * i + 1] += (r[k] + (r[k + 1] - r[k]) * f) * gr;
                s.frac += step;
            }
            const size_t used = std::min(static_cast<size_t>(s.frac), std::min(l.size(), r.size()));
            l.erase(l.begin(), l.begin() + static_cast<ptrdiff_t>(used));
            if (&r != &l)
                r.erase(r.begin(), r.begin() + static_cast<ptrdiff_t>(used));
            s.frac -= static_cast<double>(used);
        }
        for (Voice &v : g_voices)
        {
            if (!v.on || g_handles[v.handle & (kHandles - 1)].paused)
                continue;
            const Handle &h = g_handles[v.handle & (kHandles - 1)];
            const float gl = v.gl * h.volL / 128.0f * g_seVol[0] / 128.0f * mL / 32768.0f;
            const float gr = v.gr * h.volR / 128.0f * g_seVol[1] / 128.0f * mR / 32768.0f;
            const std::vector<int16_t> &pcm = v.s->pcm;
            const double len = static_cast<double>(pcm.size()), step = v.step * std::max(0.01f, h.pitch);
            for (int i = 0; i < kVblankFrames; ++i)
            {
                if (v.pos >= len)
                {
                    if (v.s->loop < 0 || len - v.s->loop < 1)
                    {
                        v.on = false;
                        break;
                    }
                    v.pos = v.s->loop + std::fmod(v.pos - len, len - v.s->loop);
                }
                const size_t k = static_cast<size_t>(v.pos);
                const size_t k1 = k + 1 < pcm.size() ? k + 1 : (v.s->loop >= 0 ? static_cast<size_t>(v.s->loop) : k);
                const float f = static_cast<float>(v.pos - k);
                float smp = pcm[k] + (pcm[k1] - pcm[k]) * f;
                if (v.release >= 0)
                {
                    smp *= static_cast<float>(v.release) / kReleaseFrames;
                    if (--v.release < 0)
                    {
                        v.on = false;
                        break;
                    }
                }
                mix[2 * i] += smp * gl;
                mix[2 * i + 1] += smp * gr;
                v.pos += step;
            }
        }
        for (float &x : mix)
            x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
        if (g_deviceOn)
            push(mix.data(), kVblankFrames);
        if (g_wav)
        {
            int16_t pcm[2 * kVblankFrames];
            for (int i = 0; i < 2 * kVblankFrames; ++i)
                pcm[i] = static_cast<int16_t>(std::lround(mix[i] * 32767.0f));
            std::fwrite(pcm, sizeof pcm, 1, g_wav);
            g_wavFrames += kVblankFrames;
            if (g_wavFrames % (kRate * 2) < kVblankFrames)   // keep the header valid every 2 s
                wavHeader();
        }
    }

    // The placeholders' answers for ROR_SOUND=0 (ror_placeholders.cpp before the host driver).
    uint32_t g_fbBank = 0, g_fbVoice = 0, g_fbStream = 0;
}

namespace ror
{
    bool soundOn() { return enabled(); }

    void soundVblank()
    {
        if (!enabled())
            return;
        if (!g_deviceTried)
        {
            openDevice();
            if (const char *w = std::getenv("ROR_SOUND_WAV"); w && *w && (g_wav = std::fopen(w, "wb")) != nullptr)
                wavHeader();
            const char *l = std::getenv("ROR_SOUND_LOG");
            g_seLog = l && *l == '1';
        }
        mixVblank();
    }
}

// ---- banks ----

void ror_snd_bank_open(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    if (!enabled())
        return ror::ret(ctx, g_fbBank++ & 0xff);
    int32_t id = ror::sarg(ctx, 0);
    const uint32_t addr = ror::arg(ctx, 1);
    if (id < 0)
    {
        id = 0;
        while (id < kBanks && g_banks[id])
            ++id;
    }
    const uint8_t *hdr = ror::ptr(rdram, addr, 0x40);
    if (id >= kBanks || g_banks[id] || !hdr || std::memcmp(hdr, "XVAB", 4) != 0)
    {
        ror::log("sound: XVAB open (id %d, %08x) refused", id, addr);
        return ror::ret(ctx, 0xffffffffu);
    }
    // header size at +8, chunks from +0xc; the body (PS-ADPCM samples) follows at the header's 16-byte-aligned end
    uint32_t hsize = 0, bodySize = 0;
    std::memcpy(&hsize, hdr + 8, 4);
    for (uint32_t p = 0xc; p + 8 <= hsize && hsize < 0x100000;)
    {
        const uint8_t *c = ror::ptr(rdram, addr + p, 16);
        if (!c)
            break;
        uint32_t size;
        std::memcpy(&size, c + 4, 4);
        if (std::memcmp(c, "HEAD", 4) == 0)
        {
            std::memcpy(&bodySize, c + 12, 4);
            break;
        }
        p += 8 + size;
    }
    const uint32_t body = (hsize + 15) & ~15u, total = body + bodySize;
    const uint8_t *all = total < (64u << 20) ? ror::ptr(rdram, addr, total) : nullptr;
    if (!all || !bodySize)
    {
        ror::log("sound: XVAB at %08x: header %#x, body %#x unreadable", addr, hsize, bodySize);
        return ror::ret(ctx, 0xffffffffu);
    }
    auto b = std::make_unique<Bank>();
    b->d.assign(all, all + total);
    b->body = body;
    for (uint32_t p = 0xc; p + 8 <= hsize;)
    {
        const uint32_t size = b->u32(p + 4);
        if (std::memcmp(&b->d[p], "HEAD", 4) == 0)
            b->head = p + 8;
        p += 8 + size;
    }
    ror::log("sound: bank %d: %u bytes (body %u)", id, total, bodySize);
    g_banks[id] = std::move(b);
    ror::ret(ctx, static_cast<uint32_t>(id));
}

void ror_snd_bank_loaded(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 1); }

void ror_snd_bank_close(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t id = ror::arg(ctx, 0);
    if (enabled() && id < kBanks && g_banks[id])
    {
        for (Voice &v : g_voices)
            if (v.on && v.bank == static_cast<int>(id))
                v.on = false;
        g_banks[id].reset();
    }
    ror::ret(ctx, 0);
}

// ---- SEs ----

void ror_snd_se_play(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (!enabled())
        return ror::ret(ctx, g_fbVoice++ & 0x3f);
    const uint32_t bank = ror::arg(ctx, 0) & 0xff, group = ror::arg(ctx, 1) & 0xff, number = ror::arg(ctx, 2) & 0xff;
    int h = -1;
    for (int i = 0; i < kHandles && h < 0; ++i)
    {
        const int c = (g_nextHandle + i) % kHandles;
        if (!g_handles[c].alloc && !handleSounding(c))
            h = c;
    }
    if (h < 0)   // all 64 in use: the next one in turn is taken over
    {
        h = g_nextHandle;
        for (Voice &v : g_voices)
            if (v.on && v.handle == h)
                v.on = false;
    }
    g_nextHandle = (h + 1) % kHandles;
    g_handles[h] = Handle{clampVol(static_cast<int16_t>(ror::arg(ctx, 3))), clampVol(static_cast<int16_t>(ror::arg(ctx, 4))),
                          1.0f, true, false};
    if (g_banks[bank])
        runSe(*g_banks[bank], static_cast<int>(bank), h, static_cast<int>(group), static_cast<int>(number));
    if (g_seLog)
    {
        std::string voices;
        for (const Voice &v : g_voices)
            if (v.on && v.handle == h && v.serial + 8 > g_serial)
                voices += " [" + std::to_string(static_cast<int>(v.step * kRate + 0.5)) + " Hz, " +
                          std::to_string(v.s->pcm.size()) + " samples" + (v.s->loop >= 0 ? ", loops" : "") + "]";
        ror::log("sound: SE bank %u group %u number %u vol %d/%d -> handle %d%s", bank, group, number,
                 g_handles[h].volL, g_handles[h].volR, h, voices.empty() ? " (silent)" : voices.c_str());
    }
    ror::ret(ctx, static_cast<uint32_t>(h));
}

void ror_snd_se_status(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t h = ror::arg(ctx, 0);
    if (!enabled() || h >= kHandles || !g_handles[h].alloc)
        return ror::ret(ctx, 0);
    if (g_handles[h].paused)
        return ror::ret(ctx, 8);
    ror::ret(ctx, handleSounding(static_cast<int>(h)) ? 1u : 5u);
}

void ror_snd_se_stop(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t h = ror::arg(ctx, 0);
    if (enabled() && h < kHandles)
    {
        stopVoices(static_cast<int>(h), -1, -1);
        g_handles[h].alloc = g_handles[h].paused = false;
    }
    ror::ret(ctx, 0);
}

// 0x23e188(handle, on, b): RPC 0x82 sub 1 with 0 (on = 0) or 1 / 2; sndman's states 4 / 6 pause and resume an SE
// with it and read status 8 back.
void ror_snd_se_pause(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t h = ror::arg(ctx, 0);
    if (enabled() && h < kHandles && g_handles[h].alloc)
        g_handles[h].paused = ror::arg(ctx, 1) != 0;
    ror::ret(ctx, 0);
}

void ror_snd_se_volume(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t h = ror::arg(ctx, 0);
    if (h < kHandles)
    {
        g_handles[h].volL = clampVol(ror::sarg(ctx, 1));
        g_handles[h].volR = clampVol(ror::sarg(ctx, 2));
    }
    ror::ret(ctx, 0);
}

void ror_snd_se_pitch(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t h = ror::arg(ctx, 0);
    if (h < kHandles && std::isfinite(ctx->f[12]))
        g_handles[h].pitch = ctx->f[12];
    ror::ret(ctx, 0);
}

// ---- streams ----

void ror_snd_stream_open(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (!enabled())
        return ror::ret(ctx, g_fbStream++ % kStreams);
    const int32_t mode = ror::sarg(ctx, 0);
    for (uint32_t i = 0; i < kStreams; ++i)
    {
        const uint32_t h = (g_fbStream + i) % kStreams;
        if (g_streams[h].used)
            continue;
        g_fbStream = h + 1;
        g_streams[h] = Stream{};
        g_streams[h].used = true;
        g_streams[h].channels = mode == 0 ? 1 : 2;
        ror::log("sound: stream %u open (%d channel%s)", h, g_streams[h].channels, mode == 0 ? "" : "s");
        return ror::ret(ctx, h);
    }
    ror::log("sound: no free ADPCM stream");
    ror::ret(ctx, 0xffffffffu);
}

void ror_snd_stream_status(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (!enabled())
        return ror::ret(ctx, 0x05);   // in use | finished
    const Stream *s = stream(ror::arg(ctx, 0));
    if (!s)
        return ror::ret(ctx, 0);
    const bool finished = s->ended && s->pcm[0].empty() && s->partial.empty();
    ror::ret(ctx, 1u | (finished ? 4u : 0u) | (s->paused ? 8u : 0u));
}

void ror_snd_stream_space(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const Stream *s = enabled() ? stream(ror::arg(ctx, 0)) : nullptr;
    if (!s || s->ended)
        return ror::ret(ctx, 0);
    const size_t queued = s->pcm[0].size();
    const size_t frames = queued < kStreamQueue ? (kStreamQueue - queued) / 28 : 0;
    const size_t bytes = frames * 16 * s->channels;
    ror::ret(ctx, static_cast<uint32_t>(bytes > s->partial.size() ? bytes - s->partial.size() : 0));
}

void ror_snd_stream_data(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    Stream *s = enabled() ? stream(ror::arg(ctx, 0)) : nullptr;
    const uint32_t addr = ror::arg(ctx, 1), n = ror::arg(ctx, 3);
    const uint8_t *src = s && n && n < (16u << 20) ? ror::ptr(rdram, addr, n) : nullptr;
    if (!src)
        return ror::ret(ctx, 0);
    // stereo: frame 2k left, 2k+1 right (str.md); mono: every frame
    s->partial.insert(s->partial.end(), src, src + n);
    const size_t group = 16u * s->channels;
    size_t p = 0;
    int16_t pcm[28];
    for (; p + group <= s->partial.size(); p += group)
        for (int c = 0; c < s->channels; ++c)
        {
            s->dec[c].frame(&s->partial[p + 16 * c], pcm, 1);
            s->pcm[c].insert(s->pcm[c].end(), pcm, pcm + 28);
        }
    s->partial.erase(s->partial.begin(), s->partial.begin() + static_cast<ptrdiff_t>(p));
    ror::ret(ctx, n);
}

void ror_snd_stream_end(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (Stream *s = enabled() ? stream(ror::arg(ctx, 0)) : nullptr)
    {
        s->ended = true;
        s->partial.clear();
    }
    ror::ret(ctx, 0);
}

void ror_snd_stream_close(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (Stream *s = enabled() ? stream(ror::arg(ctx, 0)) : nullptr)
    {
        ror::log("sound: stream %u closed%s", ror::arg(ctx, 0), s->ended ? "" : " before its end");
        *s = Stream{};
    }
    ror::ret(ctx, 0);
}

void ror_snd_stream_pause(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (Stream *s = enabled() ? stream(ror::arg(ctx, 0)) : nullptr)
        s->paused = (ror::arg(ctx, 1) & 1) != 0;
    ror::ret(ctx, 0);
}

void ror_snd_stream_volume(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (Stream *s = enabled() ? stream(ror::arg(ctx, 0)) : nullptr)
    {
        s->volL = clampVol(ror::sarg(ctx, 1));
        s->volR = clampVol(ror::sarg(ctx, 2));
    }
    ror::ret(ctx, 0);
}

void ror_snd_stream_pitch(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (Stream *s = enabled() ? stream(ror::arg(ctx, 0)) : nullptr)
        if (std::isfinite(ctx->f[12]) && ctx->f[12] > 0)
            s->pitch = ctx->f[12];
    ror::ret(ctx, 0);
}

// ---- volumes (0x23a0b0 clamps the master to -128..127, the categories to -128..128) ----

void ror_snd_master_volume(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    g_master[0] = std::min(clampVol(ror::sarg(ctx, 0)), 0x7f);
    g_master[1] = std::min(clampVol(ror::sarg(ctx, 1)), 0x7f);
    ror::ret(ctx, 0);
}

void ror_snd_se_master(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    g_seVol[0] = clampVol(ror::sarg(ctx, 0));
    g_seVol[1] = clampVol(ror::sarg(ctx, 1));
    ror::ret(ctx, 0);
}

void ror_snd_stream_master(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    g_streamVol[0] = clampVol(ror::sarg(ctx, 0));
    g_streamVol[1] = clampVol(ror::sarg(ctx, 1));
    ror::ret(ctx, 0);
}
