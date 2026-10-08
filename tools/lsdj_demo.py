#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""A demo song for the LSDj build, typed into LSDj itself: key presses for the host harness (tests/lsdj/lsdj_host.c).
The song is ours (DEADACTIVE): an A minor loop on all four channels, two phrases each: PU1 melody, PU2 bass,
WAV a held harmony voice, NOI drums (kick, snare, hats; a fill in the second phrase). No LSDj data here;
the save it produces lives in build/ with the rest of the private build (tools/lsdj_rom.py).

Editing as LSDj does it: A on an empty field inserts a value, A + RIGHT / LEFT steps it by one, A + UP / DOWN by an
octave (notes) or 0x10 (numbers); LSDj refuses a step below the lowest value, so each value is first walked down to
its floor, then up to the exact value. SELECT + RIGHT / LEFT moves song -> chain -> phrase and back.

  key_args(start_frame) -> (lsdj_host -k arguments, frames used)"""

N = {n: i for i, n in enumerate("C C# D D# E F F# G G# A A# B".split())}


def nt(name, octave):                    # C2 = 0: LSDj's lowest note
    return N[name] + 12 * (octave - 2)


def note_keys(v):
    return ["a"] + ["a+d"] * 7 + ["a+l"] * 11 + ["a+u"] * (v // 12) + ["a+r"] * (v % 12)


def num(n):                              # a chain / phrase number
    return ["a"] + ["a+d"] * 16 + ["a+l"] * 15 + ["a+r"] * n


PHRASES = {
    0: {0: nt("A", 3), 3: nt("C", 4), 6: nt("E", 4), 8: nt("D", 4), 10: nt("C", 4), 12: nt("B", 3), 14: nt("G", 3)},
    1: {0: nt("F", 3), 3: nt("A", 3), 6: nt("C", 4), 8: nt("B", 3), 10: nt("A", 3), 12: nt("G", 3), 14: nt("E", 3)},
    2: {0: nt("A", 2), 2: nt("A", 3), 4: nt("A", 2), 6: nt("A", 3), 8: nt("A", 2), 10: nt("A", 3), 12: nt("G", 2),
        14: nt("G", 3)},
    3: {0: nt("F", 2), 2: nt("F", 3), 4: nt("F", 2), 6: nt("F", 3), 8: nt("F", 2), 10: nt("F", 3), 12: nt("G", 2),
        14: nt("G", 3)},
}
# noise "notes" are LSDj's noise shapes, not pitches: picked by ear-proxy (brightness and decay measured on the host)
KICK, SNARE, HAT = 60, 30, 55
PHRASES[4] = {0: nt("E", 4), 8: nt("D", 4)}                    # WAV: over A minor, then the melody's G
PHRASES[5] = {0: nt("C", 4), 8: nt("B", 3)}                    # WAV: over F, then G
PHRASES[6] = {0: KICK, 2: HAT, 4: SNARE, 6: HAT, 8: KICK, 10: HAT, 11: KICK, 12: SNARE, 14: HAT}
PHRASES[7] = {0: KICK, 2: HAT, 4: SNARE, 6: HAT, 8: KICK, 10: SNARE, 12: SNARE, 13: SNARE, 14: SNARE, 15: SNARE}
# song row 0, one chain per channel: (chain, phrases, instrument, keys on the instrument screen, from NAME)
CHAINS = [(0, [0, 1], None, ()),          # PU1: instrument 00, LSDj's default pulse
          (1, [2, 3], None, ()),          # PU2
          (2, [4, 5], 2, ()),             # WAV: instrument 02 (LSDj makes it a WAVE instrument on this channel)
          (3, [6, 7], 3, ["d", "d"] + ["a+l"] * 4)]   # NOI: instrument 03 (NOISE), ENV 88 -> 84: a ~100 ms hit


def phrase(notes, instr=None, setup=()):
    """cursor on row 0 of the note column; ends there. instr: set on the first note (the later ones copy it), then
    `setup` keys on its instrument screen"""
    k, row = [], 0
    for r in sorted(notes):
        k += ["d"] * (r - row)
        row = r
        k += note_keys(notes[r])
        if instr is not None:
            k += ["r"] + num(instr) + (["s+r", *setup, "s+l"] if setup else []) + ["l"]
            instr, setup = None, ()
    return k + ["u"] * row


def chain(c, phrases, instr=None, setup=()):   # from the song screen, cursor on this channel's row 0; ends there
    k = num(c) + ["s+r"]
    for i, p in enumerate(phrases):
        k += num(p) + ["s+r"] + phrase(PHRASES[p], instr, setup if i == 0 else ()) + ["s+l", "d"]
    return k + ["u"] * len(phrases) + ["s+l"]


def keys():
    k = []
    for i, (c, ps, instr, setup) in enumerate(CHAINS):
        k += ["r"] * (i > 0) + chain(c, ps, instr, setup)
    return k + ["l"] * (len(CHAINS) - 1)         # back on PU1 row 0, the song stopped: START plays it


def key_args(f):
    """-k FROM:TO:KEYS for each press: a key held 4 frames, 6 frames apart; "m+k": m held, k pressed inside it"""
    out = []
    for step in keys():
        if "+" in step:
            m, k = step.split("+")
            out += ["-k", f"{f}:{f + 10}:{m}", "-k", f"{f + 3}:{f + 7}:{m}{k}"]
            f += 16
        else:
            out += ["-k", f"{f}:{f + 4}:{step}"]
            f += 10
    return out, f
