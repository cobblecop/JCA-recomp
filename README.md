# Jackie Chan Adventures — PS2 static recompilation

A static recompilation of the PAL PlayStation 2 release of *Jackie Chan
Adventures* (`SCES-52412`, Europe/Australia, En/Fr/De/Es/It/Nl/Pt), built on
[PS2Recomp](https://github.com/ran-j/PS2Recomp). The game's MIPS R5900 code is
translated to C++ once and then compiled natively — no emulator, no
interpretation at runtime.

**No game code, disc image or game assets are distributed here.** You use your
own copy of the game: a helper script pulls the game program out of your disc
image, the recompiler turns that program into C++ source on your machine, and
the build compiles it into a normal executable you can run.

## Status

It boots and renders: the intro logos, the language-select screen, the
memory-card check, the title screen and the level-loading screen all display
correctly. The Chinatown level load now gets past its loading bar and into the
first scene, where it stops on a piece of game code the translator has not
covered yet. Bringing up the rest of the game is ongoing.

Controller emulation works (PADMAN replies, and a scripted gamepad is written
into the pad area the game registers), but the game does not yet react to it on
the title and level screens, so interactivity is still being worked on.

## Build and play

Run all commands from the top folder of this project (the one containing this
README). Steps 2 and 4 are the slow ones — the final build compiles the ~33k
generated files and links a large (~1.4 GB) executable.

The build uses Ninja and (on Linux) the mold linker.

### What you need

- Your own PAL copy of the game (`SCES-52412`, Europe/Australia release),
  ripped to a plain `.iso` file (a 1:1 copy of the disc). Other rip formats
  (`.chd` or cue/bin) must be converted first — see step 1.
- CMake 3.21+, Ninja and a C++20 compiler (GCC or Clang on Linux, MSVC on
  Windows). The first configure step downloads raylib from GitHub, so you need
  an internet connection.
- On Linux, the mold linker. It is ELF-only, so it does not apply on Windows;
  the build works without it, but links are slower.
- An SSE4-capable CPU (any x86-64 desktop of the last ~15 years).
- On Linux, the OpenGL/X11 and FFmpeg development packages:

  ```bash
  sudo apt-get install -y build-essential cmake ninja-build mold pkg-config \
      libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
      libxi-dev libavcodec-dev libavformat-dev libavutil-dev \
      libswresample-dev libswscale-dev
  ```

- Python 3 (only needed for step 1).

If your distribution names packages differently,
`PS2Recomp/.github/workflows/build.yml` is the reference list.

### 1. Extract the game program from your disc image

A small script reads your `.iso` and pulls out the game program
(`SCES_524.12`), which the recompiler needs. With no argument it looks for
`game/Jackie Chan Adventures (Europe, Australia) (En,Fr,De,Es,It,Nl,Pt).iso`.

```bash
python3 tools/extract_iso.py "/path/to/Jackie Chan Adventures.iso"
# -> game/SCES_524.12   (the game/ directory is created if missing)
```

A `.chd` or raw cue/bin rip must be converted to `.iso` first, e.g. with
`chdman extractcd` followed by `bchunk`.

### 2. Build the translator

The first configure downloads raylib from GitHub, so you need internet access.
One `out/build` directory is used for every step below, so they all have to be
configured with the same generator; if you already have a directory there from
an older Makefile-based build, delete it (or pick another name for `-B` and use
that path everywhere).

```bash
cmake -S PS2Recomp -B PS2Recomp/out/build -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
cmake --build PS2Recomp/out/build --target ps2_recomp -j"$(nproc)"
```

Drop the `-DCMAKE_EXE_LINKER_FLAGS=...` argument if you do not have mold. If
`ccache` is installed, add
`-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` to
the configure line: the first build fills the cache and later rebuilds of the
generated tree hit it.

### 3. Translate the game code

Rewrites the game's code as ~33k C++ files under
`PS2Recomp/ps2xRuntime/src/runner/`. Check the function map first (it takes a
second and catches an edited or inconsistently regenerated map):

```bash
python3 tools/check_function_map.py
./PS2Recomp/out/build/ps2xRecomp/ps2_recomp configs/jca_config.toml
```

The recompiler also writes two generated tables, `ps2_recompiled_functions.h`
and `ps2_recompiled_stubs.h`, into that `runner` directory. The runtime includes
them by name from its include directory, so copy them over before building
(re-run this copy whenever you translate again):

```bash
cp PS2Recomp/ps2xRuntime/src/runner/ps2_recompiled_functions.h \
   PS2Recomp/ps2xRuntime/src/runner/ps2_recompiled_stubs.h \
   PS2Recomp/ps2xRuntime/include/
```

Both are generated files and are gitignored, so the copy leaves the repository
clean.

### 3b. Optional: recover code the function map misses

The function map in `configs/jca_map.csv` does not cover every reachable
instruction. The code it skips is never translated, and when the game jumps into
it the runtime logs `[missing-fn] target=0x...` and stops making progress
there. These steps recover it; only regenerate the map (Ghidra export, step 3)
if your game program differs from the one it was produced from.

Then split the code the map does not cover into functions (`0x110000 0x2c0000`
is this game's translatable range), translate it, and register the result:

```bash
mkdir -p PS2Recomp/ps2xRuntime/src/lib/gap_functions
python3 tools/derive_gap_map.py MAP.csv MAP_GAPS.csv 0x110000 0x2c0000 --tails
./PS2Recomp/out/build/ps2xRecomp/ps2_recomp configs/jca_config_gap.toml
cp local/artifacts/captures/output_gap/entry_gap_*.cpp \
   PS2Recomp/ps2xRuntime/src/lib/gap_functions/
python3 tools/gen_gap_registry.py
```

`configs/jca_config_gap.toml` is the template: set its `ghidra_output` to
`MAP_GAPS.csv` — the derived map from the first command, not the map you
exported, or the recompiler still sees the gaps as uncovered and translates
nothing new — and set its `output` to the scratch directory used above. After
that, build as usual; the runtime picks the handlers up through the generated
`gap_registry.inc`, and a tree without any of this still compiles and runs, it
just installs fewer recovered functions.

### 4. Build the game

Compiles the generated files and links the final executable — this is the long
step; expect roughly half an hour of compiling on a recent machine:

```bash
cmake --build PS2Recomp/out/build --target ps2EntryRunner -j"$(nproc)"
```

### 5. Run it

The runner takes the extracted game program, not the ISO:

```bash
./PS2Recomp/out/build/ps2xRuntime/ps2EntryRunner game/SCES_524.12
```

On Windows the steps are the same, but pass `--config Release` to the build
commands, replace `-j"$(nproc)"` with a plain core count (e.g. `-j8`), drop the
mold linker flag (mold is Linux/ELF only) and look for the executables in
`Release\` subdirectories of the paths above;
`PS2Recomp/.github/workflows/build.yml` shows the reference toolchain setup.
Keeping `-G Ninja` there is fine if ninja is installed and on `PATH`.

### Controls

A standard gamepad works as usual: face buttons map by position (bottom =
Cross, right = Circle, left = Square, top = Triangle), with the shoulder
buttons, triggers, Start/Select and both analog sticks where you'd expect.
Without a gamepad, the keyboard stands in:

| PS2 button | Keyboard |
| --- | --- |
| D-pad | Arrow keys |
| Left stick | W / A / S / D |
| Cross | X |
| Circle | C |
| Square | Z |
| Triangle | V |
| L1 / R1 | Q / E |
| L2 / R2 | 1 / 3 |
| L3 / R3 | Left Ctrl / Right Ctrl |
| Start / Select | Enter / Right Shift |

### About `configs/jca_config.toml`

Paths in the config are relative to the project's top folder, so run the tools
from there, as in the steps above. `input` is your extracted `SCES_524.12` and
`output` is where the translated C++ is written; neither should need editing.

`ghidra_output` points at `configs/jca_map.csv`, the function map for this
game's PAL executable. Each row is an address range with a symbol name - no
instructions, no data - produced during development by the Ghidra export
(`PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java`). It is committed
because the translated tree depends on it: without the map the recompiler
classifies from the ELF's relocations alone, prints a warning, and translates a
tree that does not boot. Verify the map against your extracted program with
`python3 tools/check_function_map.py`, which runs the recompiler's own
`--check-map` mode — the checks compile into the translator and run before it
emits anything, so a stale or half-regenerated map cannot quietly produce a
build that does not boot, and there is no second copy of the rules to drift.
Regenerate the map with the Ghidra export, or let the analyzer build a config
from scratch (build the analyzer target first):

```bash
cmake --build PS2Recomp/out/build --target ps2_analyzer -j"$(nproc)"
./PS2Recomp/out/build/ps2xAnalyzer/ps2_analyzer game/SCES_524.12 configs/jca_config.toml
```

## Debug output

A normal run stays quiet; the bring-up diagnostics are opt-in:

```bash
PS2_TRACE=1 ./PS2Recomp/out/build/ps2xRuntime/ps2EntryRunner game/SCES_524.12
```

That switch covers every per-event bring-up diagnostic in the runtime: the
work-queue traces (`[worktype]`, `[workcmd]`), GIF submits (`[gif-submit]`),
the scheduler watchdog and dumps (`[sched-watchdog]`, `[idle-dump]`,
`[dbg-inv]`, `[streamstate]`, `[dispatch-heartbeat]`), the IRQ dispatch logs
(`[irq-*]`), the SIF DMA/register dumps (`[sceSifSetDma:…]`,
`[sceSifGetReg]`, `[sceSifSetReg]`, `[rpc-pkt]`, `[bind]`, `[iop-load]`), the
GS pipeline dumps (`[gs:prim]`, `[gs:reg]`, `[gs:kick]`, `[gs:gif]`,
`[gs:copy-*]`, `[gs:packed-*]`, `[gs:texa]`, `[present-fail]`), the DMA/VIF
traces (`[chcr-kick]`, `[gif-chain]`, `[gif-tag]`, `[t2]`, `[vif1-in]`), the
APEMOD/fileio emulation traces (`[ape-*]`, `[cfio]`), the guest call trace
(`[guest-trace]`) and the display upload trace (`[frame:upload]`). IOP module
logs at Debug/Info level are also held back without it.

Finer-grained switches exist per subsystem and are independent of it — for
example `PS2_DMADBG`, `PS2_APEDBG`, `PS2_GIFTAGDBG` and `PS2_FUNC_TRACE`.
Boot progress messages (ELF load, module registration, override application)
always print.

## What's in here

| Path | Contents |
| --- | --- |
| `PS2Recomp/` | Vendored PS2Recomp toolchain and runtime (GPL-3.0, upstream history in `PS2Recomp/README.md`), plus this project's runtime fixes. |
| `PS2Recomp/ps2xRuntime/src/runner/` | The translated C++, generated on your machine by step 3. |
| `configs/` | Translator configuration: the committed function map (`jca_map.csv`) and the classification of which functions are library stubs, which are skipped, what to patch. |
| `tools/` | `extract_iso.py` — pulls the game executable out of your own disc image; `check_function_map.py` — verifies the committed function map against it. |
| `game/` | Your disc image and the executable extracted from it (created by step 1). |
| `local/` | Analysis output, logs and captures created while working on the project. |

## Legal

This project is not affiliated with, endorsed by, or sponsored by Sony
Interactive Entertainment, Atomi Entertainment, Eutechnyx, or any other rights
holder. "Jackie Chan Adventures" and all related marks and game content belong
to their respective owners.

You need to own the game to build this. The repository holds no game code or
game assets: the function map under `configs/` is a table of addresses, extents
and symbol names produced by analysing the executable, and contains no
instructions or data from it. Nothing built from the game may be redistributed.
See `NOTICE`.

The project code is released under GPL-3.0; the vendored PS2Recomp toolchain
and runtime are GPL-3.0 as well. See `LICENSE` and `PS2Recomp/LICENSE`.
