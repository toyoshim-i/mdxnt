#!/usr/bin/env python3
"""
MDX-OPN (.mdxt) MML Compiler for PC-8801mkIISR YM2203 (FM 3ch + SSG 3ch).

Follows standard MDX MML syntax and MDX-compatible compact binary command codes
extended cleanly for Yamaha YM2203 OPN:
  Channels: A, B, C = FM ch 1..3;  D, E, F = SSG ch A..C
  Header (20B): "MDXT", num_ch(6), init_tb, u16 voice_tbl_off, u16 ch_off[6]
  Commands:
    0x00..0x7F : Rest (ticks = b + 1)
    0x80..0xDF : Note (note = b - 0x80, next byte = ticks - 1)
    0xFF, tb   : Set OPN Timer-B tempo
    0xFD, v    : Set voice (FM voice idx / SSG soft-env idx 0..5)
    0xFB, vol  : Set volume (0..15)
    0xFA / 0xF9: Volume -1 / +1
    0xF8, q    : Gate time (1..8)
    0xF7       : Tie / Slur (&)
    0xF6, n, 0 : Repeat start ([)
    0xF5, s16  : Repeat end (])
    0xF4, s16  : Repeat escape (/)
    0xF3, s16  : Detune (D)
    0xF2, s16  : Pitch sweep / portamento per tick (_+N / _-N)
    0xF1, u16  : End (0) or Loop to absolute offset (L)
    0xED, m, n : SSG mixer mode (0=Tone, 1=Noise, 2=Both) + noise period (0..31)
"""

import argparse
import os
import re
import struct
import sys

NOTE_MAP = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
TPQ = 48  # ticks per quarter note (192 ticks per whole note, standard MDX/MUCOM)
WHOLE_TICKS = TPQ * 4
MAX_REP_DEPTH = 4


def bpm_to_timer_b(bpm: int) -> int:
    # YM2203 on PC-88 (3.9936 MHz, prescaler 1/6): Timer-B period = (256 - TB) * 1152 / 3993600 sec
    # For 48 ticks/beat: ticks/sec = bpm * 48 / 60 = bpm * 0.8
    # 256 - TB = 3993600 / (1152 * bpm * 0.8) = 4333.333333333333 / bpm
    bpm = max(30, min(300, bpm))
    tb = int(round(256.0 - 4333.333333333333 / bpm))
    return max(0, min(255, tb))


def pack_opn_voice(nums):
    """
    Pack an OPN 4-operator FM voice into 26 bytes:
      [0..3]   DT1/MUL (4 ops in slot order 1,3,2,4 -> reg 0x30,0x34,0x38,0x3C)
      [4..7]   TL      (reg 0x40..0x4C)
      [8..11]  KS/AR   (reg 0x50..0x5C)
      [12..15] DR      (reg 0x60..0x6C)
      [16..19] SR      (reg 0x70..0x7C)
      [20..23] SL/RR   (reg 0x80..0x8C)
      [24]     FB/CON  (reg 0xB0)
      [25]     pad (0)
    Accepts 38 ints (MUCOM/OPN order: FB, CON, then 4 ops x [AR,DR,SR,RR,SL,TL,KS,ML,DT])
    OR 47 ints (MDX @N order: CON, FL, slot_mask, then 4 ops x [AR,D1R,D2R,RR,D1L,TL,KS,MUL,DT1,DT2,AME]).
    """
    if len(nums) == 38:
        fb, con = nums[0] & 7, nums[1] & 7
        ops = [nums[2 + i * 9 : 2 + (i + 1) * 9] for i in range(4)]
        # Reorder ops from 1,2,3,4 to OPN register order 1,3,2,4
        order = [0, 2, 1, 3]
        dt_ml, tl, ks_ar, dr, sr, sl_rr = [], [], [], [], [], []
        for idx in order:
            ar, d1r, d2r, rr, sl, tlv, ks, ml, dt = ops[idx]
            dt_ml.append(((dt & 7) << 4) | (ml & 15))
            tl.append(tlv & 0x7F)
            ks_ar.append(((ks & 3) << 6) | (ar & 31))
            dr.append(d1r & 31)
            sr.append(d2r & 31)
            sl_rr.append(((sl & 15) << 4) | (rr & 15))
        return bytes(dt_ml + tl + ks_ar + dr + sr + sl_rr + [((fb & 7) << 3) | (con & 7), 0])
    elif len(nums) == 47:
        con, fb = nums[0] & 7, nums[1] & 7
        ops = [nums[3 + i * 11 : 3 + (i + 1) * 11] for i in range(4)]
        # MDX already stores ops in 1,3,2,4 order (OP1, OP3, OP2, OP4)
        dt_ml, tl, ks_ar, dr, sr, sl_rr = [], [], [], [], [], []
        for op in ops:
            ar, d1r, d2r, rr, sl, tlv, ks, ml, dt1 = op[:9]
            dt_ml.append(((dt1 & 7) << 4) | (ml & 15))
            tl.append(tlv & 0x7F)
            ks_ar.append(((ks & 3) << 6) | (ar & 31))
            dr.append(d1r & 31)
            sr.append(d2r & 31)
            sl_rr.append(((sl & 15) << 4) | (rr & 15))
        return bytes(dt_ml + tl + ks_ar + dr + sr + sl_rr + [((fb & 7) << 3) | (con & 7), 0])
    else:
        raise ValueError(f"FM voice expects 38 (OPN/MUCOM) or 47 (MDX) integers, got {len(nums)}")


def parse_length(s: str, pos: int, default_ticks: int):
    """Parse optional length (e.g. '4', '8.', '%12', '16..') returning (ticks, new_pos)."""
    n = len(s)
    if pos < n and s[pos] == "%":
        pos += 1
        m = re.match(r"(\d+)", s[pos:])
        if not m:
            raise ValueError(f"Expected tick count after '%' at {pos}")
        return max(1, int(m.group(1))), pos + len(m.group(1))
    m = re.match(r"(\d+)", s[pos:])
    if m:
        val = int(m.group(1))
        pos += len(m.group(1))
        base = WHOLE_TICKS // val if val > 0 else default_ticks
    else:
        base = default_ticks
    ticks = base
    add = base
    while pos < n and s[pos] == ".":
        add //= 2
        ticks += add
        pos += 1
    return max(1, ticks), pos


def compile_channel(mml: str, ch_idx: int, base_off: int, max_v: int = 0) -> bytes:
    """Compile one channel's MML string into MDX-compatible bytecode."""
    out = bytearray()
    octave = 4
    def_ticks = TPQ  # l4
    loop_abs_off = 0
    loop_ticks = 0
    total_ticks = 0
    rep_stack = []  # list of [start_pos, [escape_patch_positions], ticks_before_start]
    ssg_mode = 0
    ssg_nfreq = 12

    i = 0
    n = len(mml)
    while i < n:
        c = mml[i]
        if c in " \t\r\n|,":
            i += 1
            continue

        # Notes c..b
        if c in NOTE_MAP:
            i += 1
            semi = NOTE_MAP[c]
            while i < n and mml[i] in "+#-":
                if mml[i] in "+#":
                    semi += 1
                else:
                    semi -= 1
                i += 1
            note = max(0, min(95, octave * 12 + semi))
            ticks, i = parse_length(mml, i, def_ticks)
            # Support tied '^' length extension (e.g. c4^8)
            while i < n and mml[i] == "^":
                i += 1
                t2, i = parse_length(mml, i, def_ticks)
                ticks += t2
            while ticks > 256:
                out.append(0x80 + note)
                out.append(255)
                out.append(0xF7)  # tie into continuation
                ticks -= 256
                total_ticks += 256
                if loop_abs_off:
                    loop_ticks += 256
            out.append(0x80 + note)
            out.append(ticks - 1)
            total_ticks += ticks
            if loop_abs_off:
                loop_ticks += ticks
            continue

        # Rest r
        if c == "r":
            i += 1
            ticks, i = parse_length(mml, i, def_ticks)
            while i < n and mml[i] == "^":
                i += 1
                t2, i = parse_length(mml, i, def_ticks)
                ticks += t2
            while ticks > 128:
                out.append(127)
                ticks -= 128
                total_ticks += 128
                if loop_abs_off:
                    loop_ticks += 128
            out.append(ticks - 1)
            total_ticks += ticks
            if loop_abs_off:
                loop_ticks += ticks
            continue

        # Octave oN, <, >
        if c == "o":
            i += 1
            m = re.match(r"(\d+)", mml[i:])
            if not m:
                raise ValueError(f"Expected octave number after 'o'")
            octave = max(0, min(7, int(m.group(1))))
            i += len(m.group(1))
            continue
        if c == ">":
            octave = min(7, octave + 1)
            i += 1
            continue
        if c == "<":
            octave = max(0, octave - 1)
            i += 1
            continue

        # Default length lN
        if c == "l":
            i += 1
            def_ticks, i = parse_length(mml, i, def_ticks)
            continue

        # Tempo tBPM or @tTB
        if c == "t":
            i += 1
            m = re.match(r"(\d+)", mml[i:])
            if not m:
                raise ValueError("Expected BPM after 't'")
            out.extend([0xFF, bpm_to_timer_b(int(m.group(1)))])
            i += len(m.group(1))
            continue

        if c == "@":
            i += 1
            if i < n and mml[i] == "t":
                i += 1
                m = re.match(r"(\d+)", mml[i:])
                if not m:
                    raise ValueError(f"Expected Timer-B value after '@t' in ch {ch_idx}")
                out.extend([0xFF, int(m.group(1)) & 0xFF])
                i += len(m.group(1))
            elif i < n and mml[i] == "q":
                i += 1
                m = re.match(r"(\d+)", mml[i:])
                qn = max(0, min(127, int(m.group(1)))) if m else 0
                out.extend([0xF8, 0x80 | qn])
                if m:
                    i += len(m.group(1))
            else:
                m = re.match(r"(\d+)", mml[i:])
                if not m:
                    raise ValueError("Expected voice number after '@'")
                vnum = int(m.group(1))
                if ch_idx < 3 and vnum >= max_v:
                    raise ValueError(f"FM voice @{vnum} out of range (defined voices: {max_v}) in ch {ch_idx}")
                if ch_idx >= 3 and vnum >= 6:
                    raise ValueError(f"SSG envelope @{vnum} out of range (valid: 0..5) in ch {ch_idx}")
                out.extend([0xFD, vnum & 0xFF])
                i += len(m.group(1))
            continue

        # Volume vN, (, )
        if c == "v":
            i += 1
            m = re.match(r"(\d+)", mml[i:])
            if not m:
                raise ValueError("Expected volume after 'v'")
            out.extend([0xFB, max(0, min(15, int(m.group(1))))])
            i += len(m.group(1))
            continue
        if c == "(":
            out.append(0xFA)
            i += 1
            continue
        if c == ")":
            out.append(0xF9)
            i += 1
            continue

        # Gate time q1..8
        if c == "q":
            i += 1
            m = re.match(r"(\d+)", mml[i:])
            if not m:
                raise ValueError("Expected gate after 'q'")
            out.extend([0xF8, max(1, min(8, int(m.group(1))))])
            i += len(m.group(1))
            continue

        # Tie / Slur &
        if c == "&":
            out.append(0xF7)
            i += 1
            continue

        # Loop point L
        if c == "L":
            if rep_stack:
                raise ValueError(f"Loop point 'L' is not allowed inside '[...]' repeat in ch {ch_idx}")
            loop_abs_off = base_off + len(out)
            loop_ticks = 0
            i += 1
            continue

        # Repeat [ ... / ... ]N
        if c == "[":
            if len(rep_stack) >= MAX_REP_DEPTH:
                raise ValueError(f"Repeat nesting exceeds max depth {MAX_REP_DEPTH} in ch {ch_idx}")
            start_pos = len(out)
            out.extend([0xF6, 2, 0x00])  # count patched at ']'
            rep_stack.append((start_pos, [], total_ticks))
            i += 1
            continue
        if c == "/":
            if not rep_stack:
                raise ValueError("Unmatched '/' outside repeat")
            esc_pos = len(out)
            out.extend([0xF4, 0, 0])
            rep_stack[-1][1].append(esc_pos)
            i += 1
            continue
        if c == "]":
            if not rep_stack:
                raise ValueError("Unmatched ']'")
            i += 1
            m = re.match(r"(\d+)", mml[i:])
            cnt = int(m.group(1)) if m else 2
            if m:
                i += len(m.group(1))
            start_pos, escapes, start_ticks = rep_stack.pop()
            if cnt > 1 and total_ticks == start_ticks:
                raise ValueError(f"Zero-tick repeat loop '[...]{cnt}' in ch {ch_idx}")
            out[start_pos + 1] = max(1, min(255, cnt))
            end_cmd_pos = len(out)
            # Jump target is first byte AFTER [0xF6, cnt, 0x00] = start_pos + 3
            rel = (start_pos + 3) - (end_cmd_pos + 3)
            out.append(0xF5)
            out.extend(struct.pack("<h", rel))
            after_end = len(out)
            for ep in escapes:
                erel = after_end - (ep + 3)
                struct.pack_into("<h", out, ep + 1, erel)
            continue

        # Detune D[+-]N
        if c == "D":
            i += 1
            m = re.match(r"([+-]?\d+)", mml[i:])
            if not m:
                raise ValueError("Expected integer after 'D'")
            out.append(0xF3)
            out.extend(struct.pack("<h", int(m.group(1))))
            i += len(m.group(1))
            continue

        # Pitch sweep / portamento _[+-]N (per-tick delta on SSG TP or FM F-num)
        if c == "_":
            i += 1
            m = re.match(r"([+-]?\d+)", mml[i:])
            if not m:
                raise ValueError("Expected integer after '_'")
            out.append(0xF2)
            out.extend(struct.pack("<h", int(m.group(1))))
            i += len(m.group(1))
            continue

        # Software Pitch LFO: M delay,speed,depth,count (or M0 / MF0 to disable)
        if c == "M":
            i += 1
            if i < n and mml[i] in "FP":
                i += 1
            m = re.match(r"([+-]?\d+(?:\s*,\s*[+-]?\d+)*)", mml[i:])
            if not m:
                raise ValueError("Expected parameters after 'M'")
            parts = [int(x.strip()) for x in m.group(1).split(",")]
            i += len(m.group(1))
            if len(parts) == 1 and parts[0] == 0:
                out.extend([0xEE, 0, 0, 0, 0, 0])  # LFO OFF
            else:
                delay = max(0, min(255, parts[0]))
                speed = max(1, min(255, parts[1] if len(parts) > 1 else 1))
                depth = parts[2] if len(parts) > 2 else 0
                cnt = max(2, min(254, parts[3] if len(parts) > 3 else 4))
                if cnt & 1:
                    cnt = min(254, cnt + 1)
                out.append(0xEE)
                out.append(delay)
                out.append(speed)
                out.extend(struct.pack("<h", max(-32768, min(32767, depth))))
                out.append(cnt)
            continue

        # OPN SSG mixer P0/P1/P2 & noise period w0..31
        if c == "P":
            i += 1
            m = re.match(r"(\d+)", mml[i:])
            ssg_mode = max(0, min(2, int(m.group(1)))) if m else 0
            if m:
                i += len(m.group(1))
            out.extend([0xED, ssg_mode, ssg_nfreq])
            continue
        if c == "w":
            i += 1
            m = re.match(r"(\d+)", mml[i:])
            ssg_nfreq = max(0, min(31, int(m.group(1)))) if m else 12
            if m:
                i += len(m.group(1))
            out.extend([0xED, ssg_mode, ssg_nfreq])
            continue

        raise ValueError(f"Unknown MML token {c!r} at pos {i} in ch {ch_idx}: {mml[max(0,i-8):i+12]!r}")

    if rep_stack:
        raise ValueError(f"Unclosed '[' in channel {ch_idx}")
    if loop_abs_off and loop_ticks == 0:
        raise ValueError(f"Zero-tick loop after 'L' in channel {ch_idx}")

    out.append(0xF1)
    out.extend(struct.pack("<H", loop_abs_off))
    return bytes(out)


def compile_mml_text(text: str) -> bytes:
    """Compile full MDX-OPN MML source into a self-contained .mdxt binary."""
    voices = {}
    ch_mml = [""] * 6
    init_tb = bpm_to_timer_b(120)

    # Strip comments (';' to end of line)
    raw_lines = []
    for line in text.splitlines():
        semi = line.find(";")
        if semi >= 0:
            line = line[:semi]
        raw_lines.append(line.rstrip())

    # First extract multi-line @N = { ... } voice definitions
    joined = "\n".join(raw_lines)
    def _voice_sub(m):
        vnum = int(m.group(1))
        nums = [int(x) for x in re.findall(r"-?\d+", m.group(2))]
        voices[vnum] = pack_opn_voice(nums)
        return ""
    joined = re.sub(r"@(\d+)\s*=\s*\{([^}]*)\}", _voice_sub, joined, flags=re.MULTILINE)

    for line in joined.splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith("#"):
            m = re.match(r"#tempo\s+(\d+)", s, re.IGNORECASE)
            if m:
                init_tb = bpm_to_timer_b(int(m.group(1)))
            continue
        m = re.match(r"^([A-F]+)\s+(.*)$", s)
        if m:
            chs, body = m.group(1), m.group(2)
            for ch_char in chs:
                ch_mml[ord(ch_char) - ord("A")] += " " + body
            continue
        raise ValueError(f"Unrecognized MML line: {s!r}")

    # Build binary: 20B header + voice table + channel sequences
    max_v = (max(voices.keys()) + 1) if voices else 0
    voice_tbl_off = 20 if max_v > 0 else 0
    voice_blob = bytearray()
    default_voice = bytes([
        0x01, 0x01, 0x01, 0x01,
        0x18, 0x18, 0x18, 0x00,
        0x1F, 0x1F, 0x1F, 0x1F,
        0x08, 0x08, 0x08, 0x06,
        0x02, 0x02, 0x02, 0x02,
        0x27, 0x27, 0x27, 0x27,
        0x34, 0x00,
    ])
    for vi in range(max_v):
        voice_blob.extend(voices.get(vi, default_voice))

    cur_off = 20 + len(voice_blob)
    ch_offs = [0] * 6
    ch_blobs = []
    for ci in range(6):
        if ch_mml[ci].strip():
            ch_offs[ci] = cur_off
            cb = compile_channel(ch_mml[ci], ci, cur_off, max_v=max_v)
            ch_blobs.append(cb)
            cur_off += len(cb)

    hdr = bytearray()
    hdr.extend(b"MDXT")
    hdr.append(6)
    hdr.append(init_tb)
    hdr.extend(struct.pack("<H", voice_tbl_off))
    for off in ch_offs:
        hdr.extend(struct.pack("<H", off))
    return bytes(hdr) + bytes(voice_blob) + b"".join(ch_blobs)


def emit_c_header(songs: list, out_path: str):
    """Emit a C header containing named const uint8_t arrays for .mdxt binaries."""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    guard = re.sub(r"[^A-Z0-9]", "_", os.path.basename(out_path).upper())
    lines = [f"#ifndef {guard}", f"#define {guard}", "#include <stdint.h>", ""]
    for sym, data in songs:
        lines.append(f"static const uint8_t {sym}[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            chunk = ", ".join(f"0x{b:02X}u" for b in data[i : i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    lines.append(f"#endif /* {guard} */")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description="Compile MDX-OPN MML into .mdxt binary or C header")
    ap.add_argument("inputs", nargs="+", help="Input .mml files (or SYM=file.mml when using --header)")
    ap.add_argument("-o", "--out", required=True, help="Output .mdxt or .h path")
    ap.add_argument("--header", action="store_true", help="Emit C header with static const uint8_t arrays")
    args = ap.parse_args()

    if args.header:
        songs = []
        for spec in args.inputs:
            if "=" in spec:
                sym, path = spec.split("=", 1)
            else:
                path = spec
                sym = "MDXNT_" + re.sub(r"[^A-Z0-9]", "_", os.path.splitext(os.path.basename(path))[0].upper())
            data = compile_mml_text(open(path, "r", encoding="utf-8").read())
            songs.append((sym, data))
            print(f"  [mdxn] {sym:24s} <- {path} ({len(data)} B)")
        emit_c_header(songs, args.out)
        print(f"Generated {args.out} ({sum(len(d) for _, d in songs)} B total)")
    else:
        data = compile_mml_text(open(args.inputs[0], "r", encoding="utf-8").read())
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "wb") as f:
            f.write(data)
        print(f"Compiled {args.inputs[0]} -> {args.out} ({len(data)} B)")


if __name__ == "__main__":
    main()
