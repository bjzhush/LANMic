#!/usr/bin/env python3
"""Generates shared/test_vectors.txt — an independent reference encoding of the
NetMic v1 wire format. Both the C++ and Swift test suites check their codecs
against this file, so the two implementations agree byte-for-byte.

Line formats (whitespace separated, '-' = empty hex):
  valid    <name> <type> <seq> <streamId> <samplePos> <payloadHex> <packetHex>
  invalid  <name> <error> <packetHex>          error in: short magic version type length
  announce <name> <sampleRate> <channels> <format> <framesPerPacket> <nameHex> <payloadHex>
  pcm      <name> <comma-separated int16 samples> <payloadHex>
"""
import struct
from pathlib import Path

MAGIC = 0x43494D4E


def packet(ptype, seq, stream, pos, payload, version=1, magic=MAGIC, plen=None):
    if plen is None:
        plen = len(payload)
    return struct.pack("<IBBHIIQ", magic, version, ptype, plen, seq, stream, pos) + payload


def announce(rate, ch, fmt, fpp, name: bytes):
    return struct.pack("<IBBHB", rate, ch, fmt, fpp, len(name)) + name


def hx(b: bytes) -> str:
    return b.hex() if b else "-"


lines = [__doc__.strip().replace("\n", "\n# ").join(["# ", ""]), ""]

token0 = bytes(16)
token1 = bytes(range(0x10, 0x20))
ann_name = "M4 Mic".encode()
ann_cjk = "麦克风".encode()
ann1 = announce(48000, 1, 1, 240, ann_name)
ann2 = announce(48000, 2, 1, 240, ann_cjk)
pcm_samples = [0, 1, -1, 32767, -32768, 1234, -1234, 256]
pcm = struct.pack("<%dh" % len(pcm_samples), *pcm_samples)
pcm_full = struct.pack("<240h", *[(i * 137 - 16000) for i in range(240)])

valid = [
    ("discover_zero_token", 1, 0, 0, 0, token0),
    ("discover_token", 1, 7, 0, 0, token1),
    ("announce_basic", 2, 1, 0, 0, ann1),
    ("announce_utf8", 2, 0xFFFFFFFF, 0, 0, ann2),
    ("subscribe", 3, 42, 0, 0, token1),
    ("unsubscribe", 4, 43, 0, 0, token0),
    ("audio_small", 5, 1000, 0xDEADBEEF, 0x0102030405060708, pcm),
    ("audio_240", 5, 0x80000000, 0x12345678, 48000 * 3600, pcm_full),
]
for name, t, seq, sid, pos, payload in valid:
    lines.append(f"valid {name} {t} {seq} {sid} {pos} {hx(payload)} {hx(packet(t, seq, sid, pos, payload))}")

invalid = [
    ("empty", "short", b""),
    ("truncated_header", "short", packet(1, 0, 0, 0, token0)[:23]),
    ("bad_magic", "magic", packet(1, 0, 0, 0, token0, magic=0x4E4D4943)),
    ("bad_version", "version", packet(1, 0, 0, 0, token0, version=2)),
    ("unknown_type", "type", packet(9, 0, 0, 0, token0)),
    ("type_zero", "type", packet(0, 0, 0, 0, token0)),
    ("payload_declared_longer", "short", packet(1, 0, 0, 0, token0, plen=17)),
    ("payload_trailing_bytes", "length", packet(1, 0, 0, 0, token0 + b"\x00", plen=16)),
    ("discover_short_token", "length", packet(1, 0, 0, 0, token0[:15])),
    ("audio_odd_bytes", "length", packet(5, 0, 1, 0, b"\x01\x02\x03")),
    ("audio_empty", "length", packet(5, 0, 1, 0, b"")),
    ("announce_name_overrun", "length", packet(2, 0, 0, 0, ann1[:-1])),
    ("announce_too_short", "length", packet(2, 0, 0, 0, ann1[:8])),
]
for name, err, pkt in invalid:
    lines.append(f"invalid {name} {err} {hx(pkt)}")

for name, (rate, ch, fmt, fpp, nm), payload in [
    ("announce_basic", (48000, 1, 1, 240, ann_name), ann1),
    ("announce_utf8", (48000, 2, 1, 240, ann_cjk), ann2),
    ("announce_empty_name", (44100, 1, 1, 120, b""), announce(44100, 1, 1, 120, b"")),
]:
    lines.append(f"announce {name} {rate} {ch} {fmt} {fpp} {hx(nm)} {hx(payload)}")

lines.append(f"pcm pcm_edges {','.join(map(str, pcm_samples))} {hx(pcm)}")

out = Path(__file__).resolve().parent.parent / "shared" / "test_vectors.txt"
out.write_text("\n".join(lines) + "\n")
print(f"wrote {out}")
