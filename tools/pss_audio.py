"""PSS movie audio (MOVIE/*.PSS) -> SShd/SSbd stream or WAV.

A PSS is an MPEG-2 program stream. The audio sits in private stream 1 (0x1bd) packets whose payload starts with
sub-stream id 0xff and `a1 00 00`. The rest of each payload is the next piece of one Sony SShd/SSbd stream:
PS-ADPCM, usually stereo 48 kHz, channels interleaved every 0x20 bytes. ffmpeg's MPEG-PS demuxer ignores sub-id
0xff, so the port's movie player needs this demux. Evidence: notes/formats/pss.md.

Usage:
  python pss_audio.py <X.PSS> --dump
  python pss_audio.py <X.PSS> --ads <out.ads>     (raw SShd stream; ffmpeg reads it with -f ads)
  python pss_audio.py <X.PSS> --wav <out.wav>
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psadpcm  # noqa: E402

AUDIO_SUB_ID = 0xff
PREFIX_LEN = 4  # ff a1 00 00
CODEC_PSADPCM = 0x10


def audio_packets(data):
    """Yield the audio payload (prefix stripped) of each 0x1bd packet, in file order."""
    i = 0
    while True:
        i = data.find(b'\x00\x00\x01', i)
        if i < 0 or i + 9 > len(data):
            return
        sid = data[i + 3]
        if sid == 0xba:  # MPEG-2 pack header: 14 bytes + stuffing
            i += 14 + (data[i + 13] & 7)
            continue
        if sid < 0xbb:  # not a PES packet (start code inside video data)
            i += 4
            continue
        ln = int.from_bytes(data[i + 4:i + 6], 'big')
        if sid == 0xbd:
            p = data[i + 9 + data[i + 8]:i + 6 + ln]
            if p[:1] == bytes([AUDIO_SUB_ID]):
                yield p[PREFIX_LEN:]
        i += 6 + ln


def demux(data):
    """Return the whole SShd/SSbd stream (b'' when the movie has no audio)."""
    return b''.join(audio_packets(data))


def parse_header(ss):
    """SShd header -> dict, plus the offset of the ADPCM body. None if ss is not an SShd stream."""
    if ss[:4] != b'SShd':
        return None
    hsize = struct.unpack_from('<I', ss, 4)[0]
    codec, rate, nch, inter, lstart, lend = struct.unpack_from('<6I', ss, 8)
    body = 8 + hsize
    if ss[body:body + 4] != b'SSbd':
        return None
    size = struct.unpack_from('<I', ss, body + 4)[0]
    return dict(codec=codec, rate=rate, channels=nch, interleave=inter, loop_start=lstart, loop_end=lend,
                body=body + 8, size=size)


def split_channels(body, nch, interleave):
    """Block k of `interleave` bytes belongs to channel k % nch."""
    out = [bytearray() for _ in range(nch)]
    for k, p in enumerate(range(0, len(body) - interleave + 1, interleave)):
        out[k % nch] += body[p:p + interleave]
    return [bytes(c) for c in out]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('pss')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--ads')
    ap.add_argument('--wav')
    a = ap.parse_args()
    ss = demux(open(a.pss, 'rb').read())
    h = parse_header(ss)
    if not ss:
        print('no audio packets')
        return
    if h is None:
        sys.exit('audio stream does not start with SShd/SSbd')
    body = ss[h['body']:h['body'] + h['size']]
    if a.dump:
        frames = len(body) // 16
        print('SShd codec %#x%s, %d Hz, %d ch, interleave %#x, loop %#x..%#x' % (
            h['codec'], ' (PS-ADPCM)' if h['codec'] == CODEC_PSADPCM else '', h['rate'], h['channels'],
            h['interleave'], h['loop_start'], h['loop_end']))
        print('SSbd %#x bytes (%d present), %.2f s' % (h['size'], len(body),
                                                      frames // h['channels'] * 28 / h['rate']))
    if a.ads:
        open(a.ads, 'wb').write(ss)
        print('wrote %s: %d bytes' % (a.ads, len(ss)))
    if a.wav:
        if h['codec'] != CODEC_PSADPCM:
            sys.exit('codec %#x is not PS-ADPCM' % h['codec'])
        chans = [psadpcm.decode(c) for c in split_channels(body, h['channels'], h['interleave'])]
        psadpcm.write_wav(a.wav, h['rate'], chans)
        print('wrote %s: %d Hz, %d channel(s), %.2f s' % (a.wav, h['rate'], h['channels'], len(chans[0]) / h['rate']))
    if not (a.dump or a.ads or a.wav):
        ap.print_help()


if __name__ == '__main__':
    main()
