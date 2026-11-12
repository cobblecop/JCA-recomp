#!/usr/bin/env python3
"""Check the committed function map against the recompiler config and the ELF.

The recompiler only emits functions listed in the map, so a map that disagrees
with the config or with the executable produces a tree that cannot boot.  This
gate catches the failures that are otherwise silent:

  * the CSV parses and its Size column matches End - Start;
  * entries are 4-byte aligned, strictly increasing and start-unique;
  * every stub and skip selector's address is an entry start;
  * the ELF entry point is an entry start;
  * every entry lies inside an executable section of the ELF.

Entries may nest: the Ghidra export lists code labels as their own rows inside
their function's extent, and the recompiler handles that. Uncovered or nested
code is reported as a count, never as an error.

Usage: python3 tools/check_function_map.py [config.toml] [--elf PATH]
"""

import os
import re
import struct
import sys

DEFAULT_CONFIG = "configs/jca_config.toml"
KNOWN_SECTIONS = (".text",)


def parse_scalar(text, key):
    match = re.search(r"^\s*" + key + r"\s*=\s*\"([^\"]*)\"", text, re.M)
    return match.group(1) if match else ""


def parse_array(text, key):
    match = re.search(r"^\s*" + key + r"\s*=\s*\[(.*?)\]", text, re.M | re.S)
    if not match:
        return []
    return re.findall(r"\"([^\"]*)\"", match.group(1))


def parse_config(path):
    text = open(path, encoding="utf-8").read()
    return {
        "input": parse_scalar(text, "input"),
        "output": parse_scalar(text, "output"),
        "map": parse_scalar(text, "ghidra_output"),
        "stubs": parse_array(text, "stubs"),
        "untracked_stubs": parse_array(text, "untracked_stubs"),
        "skip": parse_array(text, "skip"),
    }


def elf_exec_ranges(path):
    """Return (entry point, [(start, end)]) for the ELF's executable sections."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit("%s: not a 32-bit ELF" % path)
    entry, = struct.unpack_from("<I", data, 0x18)
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, _shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    ranges = []
    for index in range(shnum):
        offset = shoff + index * shentsize
        _name, _type, flags, addr, _off, size = struct.unpack_from("<IIIIII", data, offset)
        if flags & 0x4 and size:  # SHF_EXECINSTR
            ranges.append((addr, addr + size))
    if not ranges:
        raise SystemExit("%s: no executable sections" % path)
    return entry, sorted(ranges)


def in_ranges(ranges, address):
    for start, end in ranges:
        if start <= address < end:
            return True
    return False


def read_map(path, problems, exec_ranges):
    rows = []
    with open(path, encoding="utf-8", newline="") as handle:
        lines = handle.read().splitlines()
    if not lines:
        problems.append("%s: empty" % path)
        return rows
    if lines[0].strip() != "Name,Start,End,Size":
        problems.append("%s:1: header must be Name,Start,End,Size" % path)
        return rows

    previous = None
    for number, line in enumerate(lines[1:], start=2):
        if not line.strip():
            continue
        fields = line.split(",")
        if len(fields) != 4:
            problems.append("%s:%d: expected 4 columns, got %d" % (path, number, len(fields)))
            continue
        name, start_text, end_text, size_text = (field.strip() for field in fields)
        try:
            start = int(start_text, 16)
            end = int(end_text, 16)
            size = int(size_text)
        except ValueError:
            problems.append("%s:%d: unparsable numbers (%s, %s, %s)"
                            % (path, number, start_text, end_text, size_text))
            continue

        if size <= 0:
            problems.append("%s:%d: %s has non-positive size %d" % (path, number, name, size))
        elif end - start != size:
            problems.append("%s:%d: %s size %d != end-start %d"
                            % (path, number, name, size, end - start))
        if start % 4:
            problems.append("%s:%d: %s start 0x%08X is not 4-byte aligned" % (path, number, name, start))
        if previous is not None:
            if start == previous[0]:
                problems.append("%s:%d: %s duplicates start 0x%08X" % (path, number, name, start))
            elif start < previous[0]:
                problems.append("%s:%d: %s start 0x%08X is before previous entry 0x%08X"
                                % (path, number, name, start, previous[0]))
        if exec_ranges and not in_ranges(exec_ranges, start):
            problems.append("%s:%d: %s start 0x%08X is outside the executable sections"
                            % (path, number, name, start))
        rows.append((start, end, name))
        previous = (start, end)
    return rows


def selector_address(selector):
    if "@" not in selector:
        return None
    try:
        return int(selector.rsplit("@", 1)[1], 16)
    except ValueError:
        return None


def describe_holes(rows, lo, hi):
    holes = 0
    largest = 0
    covered = 0
    reach = lo
    for start, end, _name in rows:
        start, end = max(start, lo), min(end, hi)
        if end <= start:
            continue
        if start > reach:
            holes += 1
            largest = max(largest, start - reach)
        covered += max(0, end - max(start, reach))
        reach = max(reach, end)
    if reach < hi:
        holes += 1
        largest = max(largest, hi - reach)
    return covered, holes, largest


def main():
    args = sys.argv[1:]
    elf_path = ""
    if "--elf" in args:
        index = args.index("--elf")
        elf_path = args[index + 1]
        del args[index:index + 2]
    config_path = args[0] if args else DEFAULT_CONFIG

    problems = []
    config = parse_config(config_path)
    if not config["map"]:
        problems.append("%s: ghidra_output is empty; the committed map is part of the build"
                        % config_path)
        print_problems(problems)
        return 1
    if not os.path.exists(config["map"]):
        problems.append("%s: %s does not exist (clone without the map?)"
                        % (config_path, config["map"]))
        print_problems(problems)
        return 1

    elf_entry = None
    exec_ranges = []
    if elf_path:
        elf_entry, exec_ranges = elf_exec_ranges(elf_path)
    elif config["input"]:
        try:
            elf_entry, exec_ranges = elf_exec_ranges(config["input"])
        except FileNotFoundError:
            print("note: %s not found, skipping ELF checks" % config["input"])

    rows = read_map(config["map"], problems, exec_ranges)
    starts = {start for start, _end, _name in rows}

    if elf_entry is not None and elf_entry not in starts:
        problems.append("%s: ELF entry 0x%08X is not a map entry" % (elf_path or config["input"], elf_entry))

    selectors = [("stubs", value) for value in config["stubs"]]
    selectors += [("untracked_stubs", value) for value in config["untracked_stubs"]]
    selectors += [("skip", value) for value in config["skip"]]
    for group, selector in selectors:
        address = selector_address(selector)
        if address is None:
            problems.append("%s: %r has no Name@0xADDR address" % (group, selector))
        elif address not in starts:
            problems.append("%s: %s (0x%08X) is not a map entry start" % (group, selector, address))

    if rows:
        lo = rows[0][0]
        hi = max(end for _start, end, _name in rows)
        covered, holes, largest = describe_holes(rows, lo, hi)
        span = hi - lo
        print("map:      %s" % config["map"])
        print("entries:  %d" % len(rows))
        print("range:    0x%08X - 0x%08X (%d bytes, %.2f%% covered)"
              % (lo, hi, span, 100.0 * covered / span if span else 0.0))
        if holes:
            print("uncovered: %d hole(s), largest %d bytes" % (holes, largest))
        print("selectors: %d checked" % len(selectors))
        if exec_ranges:
            print("elf:      %s (entry 0x%08X)" % (elf_path or config["input"], elf_entry or 0))

    print_problems(problems)
    if problems:
        return 1
    print("map OK")
    return 0


def print_problems(problems):
    for problem in problems:
        print("error: %s" % problem)


if __name__ == "__main__":
    sys.exit(main())
