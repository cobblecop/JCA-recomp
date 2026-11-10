#!/usr/bin/env python3
"""Derive function entries for code that the Ghidra function map does not cover.

The recompiler only emits functions listed in the map CSV, so code that Ghidra
never turned into a function is missing at runtime (the runtime reports it as
`[missing-fn] ... kind=IndirectCall`).  This script scans uncovered ranges of
the executable segment, splits them at returns or jumps that leave the range,
and writes a CSV of the recovered entries that can be fed back to
ps2_recomp via a config's ghidra_output.

Usage: python3 derive_gap_map.py BASE_MAP.csv OUT.csv LO HI [LO HI ...]
"""
import csv
import struct
import sys

ELF = "game/SCES_524.12"
NOP = 0x00000000


def load_elf(path=ELF):
    data = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', data, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
    segs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        (p_type, p_offset, p_vaddr, _pa, p_filesz,
         _msz, p_flags, _al) = struct.unpack_from('<IIIIIIII', data, o)
        if p_type == 1:
            segs.append((p_vaddr, p_filesz, p_offset, p_flags))
    return data, segs


def read(data, segs, va, n):
    for vaddr, filesz, off, _fl in segs:
        if vaddr <= va < vaddr + filesz:
            return data[off + va - vaddr: off + va - vaddr + min(n, filesz - (va - vaddr))]
    raise SystemExit(f"{va:#x} not in a segment")


def words(data, segs, lo, hi):
    body = read(data, segs, lo, hi - lo)
    return [struct.unpack_from('<I', body, i)[0] for i in range(0, len(body), 4)]


def is_return(w):
    return w == 0x03E00008  # jr $ra


def leaves_range(w, pc, lo, hi):
    """True when the instruction cannot fall through inside [lo, hi)."""
    op = w >> 26
    if op == 0x02:  # j
        target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return not (lo <= target < hi)
    if op == 0x04:  # beq
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        rt = (w >> 16) & 31
        return rt == 0 and (w >> 21) & 31 == 0 and not (lo <= pc + 4 + (simm << 2) < hi)
    return False


def split_range(data, segs, lo, hi):
    """Return [(start, end)] function extents for the uncovered range."""
    ws = words(data, segs, lo, hi)
    out = []
    start = lo
    pc = lo
    for w in ws:
        end = None
        if is_return(w) or leaves_range(w, pc, lo, hi):
            end = pc + 8  # include the delay slot
        if end is not None:
            if end <= hi:
                out.append((start, end))
            start = end
        pc += 4
    if start < hi:
        out.append((start, hi))
    # Trim leading nop padding and drop empty leftovers.  Trailing nops of the
    # last extent are kept: the last extent falls through into the next mapped
    # function, and the recompiler ends such a function with
    # `ctx->pc = <end>; return;`, so <end> has to be that function's entry.
    cleaned = []
    for i, (a, b) in enumerate(out):
        body = words(data, segs, a, b)
        while body and body[0] == NOP:
            a += 4
            body = body[1:]
        if i != len(out) - 1:
            while body and body[-1] == NOP:
                b -= 4
                body = body[:-1]
        if not body or b - a < 8:
            continue
        cleaned.append((a, b))
    if cleaned and cleaned[-1][1] < hi:
        cleaned[-1] = (cleaned[-1][0], hi)
    return cleaned



def is_exit(w, pc, lo, hi):
    """True for an instruction that ends every path through the function."""
    if is_return(w):
        return True
    op = w >> 26
    if op == 0x02:  # j
        target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return not (lo <= target < hi)
    if op == 0x04 and (w >> 16) & 31 == 0 and (w >> 21) & 31 == 0:  # b
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        return not (lo <= pc + 4 + (simm << 2) < hi)
    return False


def dead_tails(data, segs, rows):
    """Find code a map entry swallows past its own exit.

    A map entry can be sized so that the function that follows it - reachable
    only through a JALR/vtable/script dispatch - is inside the entry's extent.
    The recompiler stops translating at the entry's exit, so that unreferenced
    code is never emitted and the dispatch lands on an unmapped address.
    Return one (start, end) extent per swallowed function, repeatedly, at the
    first non-nop word after each exit.
    """
    out = []
    for r in rows:
        a, b = int(r['Start'], 16), int(r['End'], 16)
        ws = words(data, segs, a, b)
        cursor = a
        while cursor < b:
            n = (cursor - a) // 4
            exit_at = None
            for i in range(n, len(ws)):
                if is_exit(ws[i], a + 4 * i, a, b):
                    exit_at = a + 4 * i + 8
                    break
            if exit_at is None or exit_at >= b:
                break
            tail = exit_at
            j = (tail - a) // 4
            while j < len(ws) and ws[j] == NOP:
                tail += 4
                j += 1
            if tail >= b or b - tail < 8:
                break
            if not any(s <= tail < e for s, e in out):
                out.append((tail, b))
            cursor = tail
    return out

def coverage(base_map):
    cov = []
    for r in csv.DictReader(open(base_map)):
        s, e = int(r['Start'], 16), int(r['End'], 16)
        if cov and s <= cov[-1][1]:
            cov[-1] = (cov[-1][0], max(cov[-1][1], e))
        else:
            cov.append((s, e))
    return cov


def gaps(cov, lo, hi):
    out = []
    prev = lo
    for s, e in cov:
        if e <= lo or s >= hi:
            continue
        s, e = max(s, lo), min(e, hi)
        if s > prev:
            out.append((prev, s))
        prev = max(prev, e)
    if prev < hi:
        out.append((prev, hi))
    return out


def main():
    args = sys.argv[1:]
    want_tails = '--tails' in args
    args = [a for a in args if a != '--tails']
    if len(args) < 4:
        raise SystemExit(__doc__)
    base, out, ranges = args[0], args[1], args[2:]
    data, segs = load_elf()
    cov = coverage(base)
    rows = list(csv.DictReader(open(base)))
    added = 0
    for i in range(0, len(ranges), 2):
        lo, hi = int(ranges[i], 0), int(ranges[i + 1], 0)
        for g_lo, g_hi in gaps(cov, lo, hi):
            if g_hi - g_lo < 8:
                continue
            for start, end in split_range(data, segs, g_lo, g_hi):
                name = f"entry_gap_{start:08x}"
                rows.append({'Name': name,
                             'Start': f"0x{start:08X}",
                             'End': f"0x{end:08X}",
                             'Size': str(end - start)})
                print(f"{name}  0x{start:08x} - 0x{end:08x}  ({end - start} bytes)")
                added += 1
    if want_tails:
        taken = {int(r['Start'], 16) for r in rows}
        for start, end in dead_tails(data, segs, rows):
            if start in taken:
                continue
            taken.add(start)
            name = f"entry_gap_{start:08x}"
            rows.append({'Name': name,
                         'Start': f"0x{start:08X}",
                         'End': f"0x{end:08X}",
                         'Size': str(end - start)})
            print(f"{name}  0x{start:08x} - 0x{end:08x}  ({end - start} bytes, tail)")
            added += 1
    with open(out, 'w', newline='') as fh:
        writer = csv.DictWriter(fh, fieldnames=['Name', 'Start', 'End', 'Size'])
        writer.writeheader()
        writer.writerows(rows)
    print(f"{added} entries added, {len(rows)} rows written to {out}")


if __name__ == '__main__':
    main()
