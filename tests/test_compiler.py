#!/usr/bin/env python3
"""Unit tests for tools/mdxnt_compile.py."""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mdxnt_compile as mc


def test_bpm_to_timer_b():
    # YM2203 Timer-B: 256 - round(4333.3333 / bpm)
    assert mc.bpm_to_timer_b(120) == 220
    assert mc.bpm_to_timer_b(150) == 227
    assert mc.bpm_to_timer_b(60) == 184


def test_valid_compilation():
    mml = """
    #tempo 120
    @0 = {
      5, 4,
      31,10, 0, 7, 2,20,0, 1,0,
      31, 8, 2, 7, 2, 0,0, 1,0,
      31,10, 0, 7, 2,24,0, 2,0,
      31, 8, 2, 7, 2, 0,0, 1,0
    }
    A @0 o4 l8 v12 [c d [e f]2]2 L g4 a4
    D @3 o5 l16 v11 c%0 d4 M4,2,8,3 & v15 e4
    """
    data = mc.compile_mml_text(mml)
    assert data[:4] == b"MDXT"
    assert data[4] == 6
    assert data[5] == 220


def test_invalid_cases():
    v0 = "@0 = {0,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0}\n"
    cases = [
        ("missing @t", "A @t c4"),
        ("L inside repeat", "A [L c4]2"),
        ("0-tick repeat", "A [v12]2 c4"),
        ("0-tick L loop", "A c4 L"),
        ("repeat depth > 4", "A [[[[[c4]2]2]2]2]2"),
        ("FM voice OOB with voices", v0 + "A @1 c4"),
        ("FM voice OOB with 0 voices", "A @0 c4"),
        ("SSG envelope OOB", "D @6 c4"),
    ]
    for name, src in cases:
        try:
            mc.compile_mml_text(src)
            raise AssertionError(f"Expected ValueError for: {name}")
        except ValueError:
            pass


if __name__ == "__main__":
    test_bpm_to_timer_b()
    test_valid_compilation()
    test_invalid_cases()
    print("ALL COMPILER TESTS PASSED!")
