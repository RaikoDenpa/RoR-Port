// Placeholders that always report "finished", until the real movie, sound and memory-card seams exist
// (notes/port-strategy.md 3; the pad is real now, ror_pad.cpp). Each keeps the contract its callers check:
//   rxmpeg open 0x18313c      every movie goes through the player 0x7ddfc0: TskMovie (through 0x7aa738) and the title
//                             attract mode 0x1aaae4, which bypasses TskMovie. Its open must return >= 0 (0x181c3c
//                             retries until then), then ready 0x29af7c (0x1832cc) and finished 0x29af58 (0x1837c0)
//                             are what both poll. TskMovie::exec itself runs: its start/close 0x193150/0x19319c push
//                             and pop a MagSystem submode (0x1328b0/0x1328f0) that its term relies on
//   rxsnd client              ids in range (voices < 0x40 per 0x23e150, ADPCM < 0x30 per 0x240080, banks < 0x100
//                             per 0x23c648); sndman 0x180900 closes a stream once status bit 0x4 is set
//   libmc                     each call is accepted (0) and sceMcSync 0x24fb98 then returns 1 with the result;
//                             -1 when nothing is pending, as the original does
#include "ror.h"

#include <string>

// ---- movies ----

void ror_rxmpeg_open(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    // The state the per-frame update 0x1832d8 leaves at the end of a stream: prebuffered (ready), past the last
    // 0x2800 bytes (0x29af94, the title's fade cue in 0x181e50), no data left, finished. The stream handle stays -1,
    // so the update and start 0x18324c do nothing, and no decode worker (0x182824) is queued.
    ror::log("movie movie/%s skipped (rxmpeg placeholder)", ror::str(rdram, ror::arg(ctx, 0), 0x40).c_str());
    ror::wr32(rdram, 0x29af44, 0xffffffffu);  // stream handle
    ror::wr32(rdram, 0x29af48, 0);           // bytes left
    ror::wr32(rdram, 0x29af58, 1);           // finished
    ror::wr32(rdram, 0x29af7c, 1);           // ready
    ror::wr32(rdram, 0x29af94, 1);           // end reached
    ror::ret(ctx, 0);
}

// ---- rxsnd ----

namespace
{
    uint32_t g_bank = 0, g_voice = 0, g_adpcm = 0;
}

void ror_snd_bank_open(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, g_bank++ & 0xff); }
void ror_snd_one(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 1); }
void ror_snd_voice_play(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, g_voice++ & 0x3f); }
void ror_snd_adpcm_open(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, g_adpcm++ % 0x30); }
void ror_snd_adpcm_status(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0x05); }  // in use | finished

// ---- libmc: an empty, formatted 8 MB card in slot 1 (port 0), no card in slot 2 ----
// Result codes and function numbers follow libmc (sceMcResSucceed 0, ChangedCard -1, NoEntry -4; GetInfo 1 ...
// Rename 0x13; 0xb and 0x13 also appear in 0x24f680 / 0x250648). The no-card code -10 is UNVERIFIED.

namespace
{
    struct Pending
    {
        bool pending = false;
        int32_t cmd = 0;
        int32_t result = 0;
    } g_mc;
    bool g_mcSeen[2] = {};

    void accept(R5900Context *ctx, int32_t cmd, int32_t result)
    {
        g_mc = {true, cmd, result};
        ror::ret(ctx, 0);
    }
}

void ror_sceMcInit(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }

void ror_sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = ror::arg(ctx, 0) & 1;
    const uint32_t type = ror::arg(ctx, 2), freeOut = ror::arg(ctx, 3), format = ror::arg(ctx, 4);
    if (type)
        ror::wr32(rdram, type, port == 0 ? 2 : 0);  // 2 = PS2 card
    if (freeOut)
        ror::wr32(rdram, freeOut, port == 0 ? 8000 : 0);
    if (format)
        ror::wr32(rdram, format, port == 0 ? 1 : 0);
    const int32_t result = port == 0 ? (g_mcSeen[0] ? 0 : -1) : -10;
    g_mcSeen[port] = true;
    accept(ctx, 1, result);
}

void ror_sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    ror::log("sceMcOpen(%u, %s, mode %#x): empty card", ror::arg(ctx, 0), ror::str(rdram, ror::arg(ctx, 2)).c_str(), ror::arg(ctx, 3));
    accept(ctx, 2, -4);
}

void ror_sceMcClose(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 3, -4); }
void ror_sceMcSeek(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 4, -4); }
void ror_sceMcRead(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 5, -4); }
void ror_sceMcWrite(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 6, -4); }
void ror_sceMcFlush(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 0xa, 0); }
void ror_sceMcMkdir(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 0xb, 0); }

void ror_sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const std::string dir = ror::str(rdram, ror::arg(ctx, 2));
    if (const uint32_t cur = ror::arg(ctx, 3))
        ror::wr16(rdram, cur, '/');  // "/" and its terminator
    accept(ctx, 0xc, (dir == "/" || dir.empty()) ? 0 : -4);
}

void ror_sceMcGetDir(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 0xd, 0); }  // 0 entries
void ror_sceMcDelete(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 0xf, -4); }
void ror_sceMcFormat(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 0x10, 0); }
void ror_sceMcUnformat(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 0x11, 0); }
void ror_sceMcGetEntSpace(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 0x12, 1000); }
void ror_sceMcRename(uint8_t *, R5900Context *ctx, PS2Runtime *) { accept(ctx, 0x13, -4); }

void ror_sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    if (!g_mc.pending)
    {
        ror::ret(ctx, static_cast<uint32_t>(-1));
        return;
    }
    if (const uint32_t cmd = ror::arg(ctx, 1))
        ror::wr32(rdram, cmd, static_cast<uint32_t>(g_mc.cmd));
    if (const uint32_t result = ror::arg(ctx, 2))
        ror::wr32(rdram, result, static_cast<uint32_t>(g_mc.result));
    g_mc.pending = false;
    ror::ret(ctx, 1);
}
