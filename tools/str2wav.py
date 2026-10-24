"""STR sound streams (DATA/SND/XWS/S######.STR) -> WAV.

A stream is headerless PS-ADPCM at 48 kHz. Ids below 100000 (voices, ambience) are mono. Ids from 100000 up (music)
are stereo with 16-byte frame interleave (L frame, R frame, ...). Evidence: notes/formats/str.md (gamSnd 0x1e66a0 and
0x180428 pick ADPCM stream mode 0 / 2 by id; 0x240180 splits mode 2 frame by frame; sndman 0x180900 opens it at
48000 Hz). The game clears every frame's flag byte before upload, so in-file loop flags are ignored.

Usage:
  python str2wav.py <S######.STR> --dump
  python str2wav.py <S######.STR> --wav <out.wav> [--mono | --stereo]
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psadpcm  # noqa: E402

RATE = 48000
STEREO_FROM_ID = 100000


def stream_id(path):
    m = re.match(r's(\d+)\.str$', os.path.basename(path), re.I)
    return int(m.group(1)) if m else None


def channels_for(path, force=None):
    if force:
        return force
    sid = stream_id(path)
    return 2 if sid is not None and sid >= STEREO_FROM_ID else 1


def split_channels(data, nch):
    """Frame interleave: frame k belongs to channel k % nch."""
    if nch == 1:
        return [data]
    out = [bytearray() for _ in range(nch)]
    for k, p in enumerate(range(0, len(data) - 15, 16)):
        out[k % nch] += data[p:p + 16]
    return [bytes(c) for c in out]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('stream')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--wav')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--mono', action='store_const', const=1, dest='force')
    g.add_argument('--stereo', action='store_const', const=2, dest='force')
    a = ap.parse_args()
    data = open(a.stream, 'rb').read()
    if len(data) % 16:
        sys.exit('size %d is not a whole number of 16-byte ADPCM frames' % len(data))
    nch = channels_for(a.stream, a.force)
    frames = len(data) // 16
    if a.dump:
        flags = {}
        for k in range(frames):
            f = data[k * 16 + 1]
            if f:
                flags.setdefault(f, []).append(k)
        print('id %s, %d bytes, %d frames, %s, %.2f s at %d Hz' % (
            stream_id(a.stream), len(data), frames, 'stereo (16-byte interleave)' if nch == 2 else 'mono',
            frames // nch * 28 / RATE, RATE))
        for f, ks in sorted(flags.items()):
            print('  frame flags %#04x: %d frames, first %s' % (f, len(ks), ks[:4]))
    if a.wav:
        chans = [psadpcm.decode(c) for c in split_channels(data, nch)]
        psadpcm.write_wav(a.wav, RATE, chans)
        print('wrote %s: %d Hz, %d channel(s), %.2f s' % (a.wav, RATE, nch, len(chans[0]) / RATE))
    if not (a.dump or a.wav):
        ap.print_help()


if __name__ == '__main__':
    main()
