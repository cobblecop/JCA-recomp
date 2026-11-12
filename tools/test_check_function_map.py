#!/usr/bin/env python3
"""Test that the function-map gate accepts the committed map and rejects bad ones.

The gate is `ps2_recomp --check-map <config>`, which runs the same validation the
translator uses before writing anything (rules live in ps2_recompiler.cpp). This
script feeds it the committed map, then a handful of deliberately broken copies,
and checks that

  * the committed map still passes (`map OK`),
  * every broken copy fails with an error that names the map and a line number,
  * a direct translation with the same broken config refuses to translate and
    writes no runner code.

Build the recompiler first (README step 2); `PS2_RECOMP` overrides its path.

Usage: python3 tools/test_check_function_map.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

RECOMP = os.path.join("PS2Recomp", "out", "build", "ps2xRecomp", "ps2_recomp")
CONFIG = "configs/jca_config.toml"
MAP = "configs/jca_map.csv"


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_recompiler():
    override = os.environ.get("PS2_RECOMP")
    if override:
        return override
    return os.path.join(repo_root(), RECOMP)


def read_map(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read().splitlines()


def write_map(path, lines):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def config_for(map_path, output_dir, path):
    with open(os.path.join(repo_root(), CONFIG), "r", encoding="utf-8") as handle:
        text = handle.read()
    text = re.sub(r'(?m)^ghidra_output\s*=.*$', 'ghidra_output = "%s"' % map_path, text)
    text = re.sub(r'(?m)^output\s*=.*$', 'output = "%s"' % output_dir, text)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def run(recompiler, args):
    result = subprocess.run(
        [recompiler] + args,
        cwd=repo_root(),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result.returncode, result.stdout


def broken_maps():
    """Each entry: (name, mutator, expected fragment of the refused reason)."""
    lines = read_map(os.path.join(repo_root(), MAP))
    header = lines[0]
    rows = lines[1:]

    def size_mismatch():
        copy = list(lines)
        for index in range(1, len(copy)):
            cell = copy[index].split(",")
            if len(cell) == 4:
                cell[3] = str(int(cell[3]) + 4)
                copy[index] = ",".join(cell)
                return copy
        raise AssertionError("no sized row in %s" % MAP)

    def misaligned_start():
        copy = list(lines)
        cell = copy[2].split(",")
        address = int(cell[1], 16)
        cell[1] = "0x%08X" % (address + 1)
        copy[2] = ",".join(cell)
        return copy

    def out_of_order():
        copy = list(lines)
        copy[2], copy[3] = copy[3], copy[2]
        return copy

    def bad_header():
        copy = list(lines)
        copy[0] = "Name,Start,End"
        return copy

    assert len(rows) > 10
    assert header.startswith("Name,"), header
    return [
        ("size_mismatch", size_mismatch()),
        ("misaligned_start", misaligned_start()),
        ("out_of_order", out_of_order()),
        ("bad_header", bad_header()),
    ]


def main():
    recompiler = find_recompiler()
    if not os.path.isfile(recompiler):
        print("error: %s not found; build it first (README step 2) or set PS2_RECOMP" % recompiler)
        return 2

    failures = []
    root = repo_root()

    rc, out = run(recompiler, ["--check-map", os.path.join(root, CONFIG)])
    if rc != 0 or "map OK" not in out:
        failures.append("committed map: expected rc 0 and 'map OK', got rc %d\n%s" % (rc, out))
        print("FAIL committed map")
    else:
        print("ok   committed map passes")

    for name, lines in broken_maps():
        work = tempfile.mkdtemp(prefix="mapgate-%s-" % name)
        try:
            map_path = os.path.join(work, "bad_%s.csv" % name)
            write_map(map_path, lines)
            output_dir = os.path.join(work, "runner")
            config_path = os.path.join(work, "bad.toml")
            config_for(map_path, output_dir, config_path)

            rc, out = run(recompiler, ["--check-map", config_path])
            located = re.search(re.escape(map_path) + r":\d+: ", out)
            if rc == 0 or not located:
                failures.append(
                    "%s: expected rc != 0 with a file:line error, got rc %d\n%s" % (name, rc, out)
                )
                print("FAIL %s (check-map)" % name)
            else:
                print("ok   %s rejected with a file:line error" % name)

            rc, out = run(recompiler, [config_path])
            if rc == 0 or "Refusing to translate" not in out:
                failures.append(
                    "%s: translation was not refused, rc %d\n%s" % (name, rc, out)
                )
                print("FAIL %s (translate)" % name)
            else:
                written = []
                for directory, _, files in os.walk(output_dir):
                    written.extend(f for f in files if f.endswith((".cpp", ".h")))
                if written:
                    failures.append("%s: translation wrote %d files despite refusing" % (name, len(written)))
                    print("FAIL %s (wrote output)" % name)
                else:
                    print("ok   %s refused to translate, wrote nothing" % name)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    # A selector must exist in the map: point a stub at an address the map does
    # not describe and the gate has to refuse the config as well.
    work = tempfile.mkdtemp(prefix="mapgate-selector-")
    try:
        map_path = os.path.join(repo_root(), MAP)
        output_dir = os.path.join(work, "runner")
        config_path = os.path.join(work, "bad.toml")
        config_for(map_path, output_dir, config_path)
        with open(config_path, "r", encoding="utf-8") as handle:
            text = handle.read()
        text = text.replace("SetGsCrt@0x002B7820", "SetGsCrt@0x002B7824")
        with open(config_path, "w", encoding="utf-8") as handle:
            handle.write(text)

        rc, out = run(recompiler, ["--check-map", config_path])
        if rc == 0 or "is not a map entry start" not in out:
            failures.append("missing_selector: expected refusal, got rc %d\n%s" % (rc, out))
            print("FAIL missing_selector")
        else:
            print("ok   missing_selector rejected")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print("\n%d failure(s):\n" % len(failures))
        for failure in failures:
            print(failure.rstrip() + "\n")
        return 1
    print("\nall map gate checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
