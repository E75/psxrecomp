#!/usr/bin/env python3
"""Codegen test: bounded tables with the SLL in the bounds BEQ's delay slot.

Two shapes from the retail library next door (WE2002's SELECT2/GAME overlays)
were left as a bare CPS tail-transfer, so the trampoline had to dispatch an
interior case label and the dirty-RAM interpreter ran the rest of the screen:

  SELECT2: sltiu v0,v1,148; beq v0,zero,out; sll v0,v1,2;
           lui at,0x800D; addu at,at,v0; lw v0,-2244(at); nop; jr v0
  GAME:    sltiu v0,v1,3; beq v0,zero,out; lui s4,0x1F80; sll v0,v1,2;
           lui at,0x8001; addu at,at,v0; lw v0,64(at); nop; jr v0

In both, the SLL is not the instruction immediately before ADDU: in the first
it is the guard's delay slot, in the second an unrelated LUI sits in the slot
and the SLL follows it. The tables are also allowed to sit outside the
function and the image length is not a multiple of four.

Expected: a `/* jump table ... */` switch over every entry, each a
`goto block_...`, and an interior `block_...:` label for a target that is not
a block start elsewhere.

Usage:  python test_index_in_guard_slot_jump_codegen.py [--recompiler <exe>]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

LOAD = 0x80010000


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, data, tail=0):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, LOAD)
    struct.pack_into("<I", h, 0x1C, len(data) + tail)
    return bytes(h) + data + b"\x00" * tail


def jump_table_fn(entry, guard_slot_lui=False):
    """Three-entry table; returns (code bytes, table address, case pcs).

    Layout (byte offsets): 00 sltiu, 04 beq(out=28), 08/0C slope, 1C/20 jr,
    28 out + conditional fall-through so the case blocks stay in the walked
    function (as the real switch does), 34/40/4C cases, 58 jr ra, 60 table.
    """
    table = entry + 0x60
    case0, case1, case2 = entry + 0x34, entry + 0x40, entry + 0x4C
    exit_pc = entry + 0x58
    word = [0] * 0x20
    lw_word = 0x8C220000 | ((table - LOAD) & 0xFFFF)
    j_exit = 0x08000000 | ((exit_pc >> 2) & 0x03FFFFFF)

    def at(byte_off, value):
        word[byte_off // 4] = value

    at(0x00, 0x2C620003)               # sltiu v0,v1,3
    at(0x04, 0x10400008)               # beq v0,zero, +0x28
    if guard_slot_lui:
        at(0x08, 0x3C141F80)           # lui s4,0x1F80 (unrelated delay slot)
        at(0x0C, 0x00031080)           # sll v0,v1,2
        at(0x10, 0x3C018001)           # lui at,0x8001
        at(0x14, 0x00220821)           # addu at,at,v0
        at(0x18, lw_word)              # lw v0,table(at)
        at(0x1C, 0)
        at(0x20, 0x00400008)           # jr v0
        at(0x24, 0)
    else:
        at(0x08, 0x00031080)           # sll v0,v1,2 (guard delay slot)
        at(0x0C, 0x3C018001)           # lui at,0x8001
        at(0x10, 0x00220821)           # addu at,at,v0
        at(0x14, lw_word)              # lw v0,table(at)
        at(0x18, 0)
        at(0x1C, 0x00400008)           # jr v0
        at(0x20, 0)
    at(0x28, 0x24020009)               # out: addiu v0,zero,9
    at(0x2C, 0x1480000A)               # bne a0,zero, exit (fall through)
    at(0x30, 0)
    for pc, value in ((case0, 1), (case1, 2), (case2, 3)):
        at(pc - entry, 0x24020000 | value)
        at(pc - entry + 4, j_exit)
        at(pc - entry + 8, 0)
    at(0x58, 0x03E00008)               # exit: jr ra
    at(0x5C, 0)
    at(0x60, case0)
    at(0x64, case1)
    at(0x68, case2)
    return w(word), table, (case0, case1, case2)


def build_exe():
    a, table_a, cases_a = jump_table_fn(LOAD)
    fn2 = LOAD + len(a)
    b, table_b, cases_b = jump_table_fn(fn2, guard_slot_lui=True)
    data = a + b
    return make_psxexe(LOAD, data, tail=1), (table_a, cases_a), (table_b, cases_b)


def gen_c(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    exe, t1, t2 = build_exe()
    with open(psx, "wb") as f:
        f.write(exe)
    with open(seeds, "w") as f:
        f.write("0x%08X\n0x%08X\n" % (LOAD, LOAD + 0x80))
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"recompiler failed:\n{r.stderr or r.stdout}")
    chunks = []
    for name in sorted(os.listdir(out)):
        if "_full" in name and name.endswith(".c") and "_dispatch" not in name:
            with open(os.path.join(out, name), encoding="utf-8") as f:
                chunks.append(f.read())
    return "\n".join(chunks), t1, t2


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler", default=os.path.normpath(
        os.path.join(here, "..", "build", "psxrecomp-game.exe")))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")
    with tempfile.TemporaryDirectory() as tmp:
        c, (table_a, cases_a), (table_b, cases_b) = gen_c(args.recompiler, tmp)
    fails = []
    for tag, table, cases in (("guard-slot", table_a, cases_a), ("unrelated-slot", table_b, cases_b)):
        if not re.search(r"/\* jump table 0x%08X \(rom 0x%08X\), 3 entries \*/" % (table, table), c):
            fails.append(f"{tag}: no jump-table switch comment for 0x{table:08X}")
        for t in cases:
            if not re.search(r"case 0x%08Xu:\s*(?:\n.*?)*?goto block_%08X;" % (t, t), c):
                fails.append(f"{tag}: missing case 0x{t:08X} -> goto block_{t:08X}")
        if not re.search(r"switch \(_jt_", c):
            fails.append(f"{tag}: no switch over the table")
        for t in cases:
            if ("block_%08X:" % t) not in c:
                fails.append(f"{tag}: no interior label block_{t:08X}")
            # An interior arm is also a computed-jump target from another
            # image, so it needs an entry-switch dispatch key as well.
            if not re.search(r"case 0x%08Xu:\s*(?:\n.*?)*?goto block_%08X;" % (t, t), c):
                fails.append(f"{tag}: no entry-switch case for interior arm 0x{t:08X}")
    for f in fails:
        print("FAIL:", f)
    if fails:
        return 1
    print("PASS: both guard-delay-slot table shapes emit a switch over every case.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

