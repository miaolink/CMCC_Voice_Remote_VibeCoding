#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Capture CMCC 0xFC audio from ADV CDC → dewhite → G.722.1 WAV.

Compatible with cmcc-ble-bridge Serial lines:
  # VOICE 1 / # VOICE 0
  A <40 hex chars of 20B notify>
  # A START / # A END

Usage:
  python voice_cap.py COM3 --auto
  python voice_cap.py list
"""
from __future__ import annotations

import argparse
import datetime
import os
import struct
import subprocess
import sys
import time

IV_DEC_TABLE = [4, 16, 9, 8, 3, 11, 2, 6, 18, 7, 13, 17]
XOR_KEY = 0x0416
CHUNK_DATA, CHUNK_HEAD = 16, 4
CODEC_BYTES, WORDS = 40, 20
HERE = os.path.dirname(os.path.abspath(__file__))


def dewhite(frame48: bytes) -> bytes:
    words = list(struct.unpack("<20H", frame48[:CODEC_BYTES]))
    for g in range(4):
        a, b, c = IV_DEC_TABLE[3 * g], IV_DEC_TABLE[3 * g + 1], IV_DEC_TABLE[3 * g + 2]
        oa, ob, oc = words[a], words[b], words[c]
        words[a], words[b], words[c] = oc, oa, ob
    words = [w ^ XOR_KEY for w in words]
    out = bytearray(CODEC_BYTES)
    for i, w in enumerate(words):
        out[i * 2] = (w >> 8) & 0xFF
        out[i * 2 + 1] = w & 0xFF
    return bytes(out)


class Framer:
    def __init__(self):
        self.slots = {}
        self.frames = 0
        self.gap = 0
        self.last_seq = None
        self.on_frame = None

    def reset(self):
        self.slots.clear()
        self.frames = 0
        self.gap = 0
        self.last_seq = None

    def feed(self, payload: bytes):
        if len(payload) < CHUNK_HEAD + CHUNK_DATA:
            return None
        seq = payload[0] | (payload[1] << 8)
        idx = payload[2]
        if idx > 2:
            return None
        if idx == 0:
            if self.last_seq is not None and seq != ((self.last_seq + 1) & 0xFFFF):
                self.gap += 1
            self.last_seq = seq
        if len(self.slots) > 200:
            self.slots.clear()
        s = self.slots.setdefault(seq, {})
        s[idx] = payload[CHUNK_HEAD : CHUNK_HEAD + CHUNK_DATA]
        if len(s) < 3:
            return None
        frame = s.get(0, b"") + s.get(1, b"") + s.get(2, b"")
        del self.slots[seq]
        self.frames += 1
        if self.on_frame:
            self.on_frame(frame)
        return frame


def find_decoder():
    for p in (
        os.path.join(HERE, "decode.exe"),
        os.path.join(HERE, "libg7221", "decode.exe"),
    ):
        if os.path.exists(p):
            return p
    return None


def list_ports():
    from serial.tools import list_ports

    for p in list_ports.comports():
        vidpid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else ""
        print(f"{p.device:8} {vidpid:10} {p.description}")


def decode_to_wav(raw_frames, out_base, decoder):
    if not raw_frames:
        print("# no frames")
        return None
    codec = b"".join(dewhite(f) for f in raw_frames)
    raw_path = out_base + ".g7221.raw"
    wav_path = out_base + ".wav"
    with open(raw_path, "wb") as f:
        f.write(codec)
    print(f"# wrote {raw_path} ({len(codec)} bytes, {len(raw_frames)} frames)")
    if not decoder:
        print("# decode.exe missing — run tools/voice/build_decode.bat")
        return raw_path
    r = subprocess.run([decoder, raw_path, wav_path, "16000"], capture_output=True, text=True)
    print(r.stderr.strip() or r.stdout.strip())
    if r.returncode == 0 and os.path.exists(wav_path):
        print(f"# OK {wav_path}")
        return wav_path
    print("# decode failed")
    return raw_path


def capture(port, secs=60.0, auto=True, baud=115200):
    import serial

    decoder = find_decoder()
    raw_frames = []
    framer = Framer()
    framer.on_frame = lambda f: raw_frames.append(f)

    ser = None
    for _ in range(8):
        try:
            ser = serial.Serial(port, baud, timeout=0.2)
            break
        except Exception as e:
            last = e
            time.sleep(0.4)
    if ser is None:
        print(f"# open fail {port}: {last}")
        return

    print(f"# open {port}. Hold voice key, speak, release.")
    print("# waiting for # VOICE 1 / A lines ...")
    t_end = time.time() + secs
    line_buf = b""
    voice_on = False
    voice_off_t = None
    last_a = None
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_base = os.path.join(HERE, "out", f"voice_{stamp}")
    os.makedirs(os.path.join(HERE, "out"), exist_ok=True)

    try:
        while time.time() < t_end:
            n = ser.in_waiting
            chunk = ser.read(n if n else 1)
            if not chunk:
                if auto and voice_off_t and (time.time() - voice_off_t) > 2.5 and last_a and (
                    time.time() - last_a
                ) > 2.5:
                    break
                continue
            line_buf += chunk
            while b"\n" in line_buf:
                line, line_buf = line_buf.split(b"\n", 1)
                s = line.decode("utf-8", "replace").strip()
                if s.startswith("A "):
                    hx = s[2:].replace(" ", "")
                    try:
                        payload = bytes.fromhex(hx)
                    except ValueError:
                        continue
                    framer.feed(payload)
                    last_a = time.time()
                elif s.startswith("# VOICE 1"):
                    voice_on = True
                    voice_off_t = None
                    framer.reset()
                    raw_frames.clear()
                    print("# VOICE down — capturing")
                elif s.startswith("# VOICE 0"):
                    voice_on = False
                    voice_off_t = time.time()
                    print(f"# VOICE up — frames so far {framer.frames} gap={framer.gap}")
                elif s.startswith("# A END") and auto and voice_off_t:
                    # keep waiting post_idle for late frames
                    pass
                elif s.startswith("# A START"):
                    print(s)
    finally:
        ser.close()

    print(f"# done frames={framer.frames} gap={framer.gap} raw={len(raw_frames)}")
    decode_to_wav(raw_frames, out_base, decoder)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", nargs="?", default="cap", choices=["cap", "list"])
    ap.add_argument("port", nargs="?", default="COM3")
    ap.add_argument("--secs", type=float, default=60.0)
    ap.add_argument("--auto", action="store_true", default=True)
    args = ap.parse_args()
    if args.cmd == "list":
        list_ports()
    else:
        capture(args.port, secs=args.secs, auto=args.auto)


if __name__ == "__main__":
    main()
