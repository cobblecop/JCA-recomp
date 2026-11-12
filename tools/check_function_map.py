#!/usr/bin/env python3
"""Check the function map (delegates to the recompiler).

The rules live in one place: ps2_recompiler.cpp validates the map before it
translates anything, and this script runs the same code through
`ps2_recomp --check-map`, so the developer check and the translator can never
disagree. It reports the entries, range, coverage and holes, then every problem
with its file:line, and exits non-zero if the map is unusable.

Build the recompiler first (README step 2); `PS2_RECOMP` overrides its path.

Usage: python3 tools/check_function_map.py [config.toml]

The config's `input` and `ghidra_output` decide what is checked: the map is
validated as a whole, and every stub, skip and entry-point selector must exist
in it.
"""

import os
import subprocess
import sys

DEFAULT_CONFIG = "configs/jca_config.toml"
RECOMP = os.path.join("PS2Recomp", "out", "build", "ps2xRecomp", "ps2_recomp")


def find_recompiler():
    override = os.environ.get("PS2_RECOMP")
    if override:
        return override
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, RECOMP)


def main():
    config = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CONFIG
    recompiler = find_recompiler()
    if not os.path.isfile(recompiler):
        print("error: %s not found; build it first (README step 2) or set PS2_RECOMP" % recompiler)
        return 2

    result = subprocess.run([recompiler, "--check-map", config], stdin=subprocess.DEVNULL)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
