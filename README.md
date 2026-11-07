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

It boots and renders, controller input works, and the language-select and
memory-card screens behave. Bringing up the rest of the game is ongoing.

## Build and play

Run all commands from the top folder of this project (the one containing this
README). Steps 2 and 4 are the slow ones — the final build compiles the ~33k
generated files and links a large (~1.4 GB) executable.

### What you need

- Your own PAL copy of the game (`SCES-52412`, Europe/Australia release),
  ripped to a plain `.iso` file (a 1:1 copy of the disc). Other rip formats
  (`.chd` or cue/bin) must be converted first — see step 1.
- CMake 3.21+ and a C++20 compiler (GCC or Clang on Linux, MSVC on Windows).
  The first configure step downloads raylib from GitHub, so you need an
  internet connection.
- An SSE4-capable CPU (any x86-64 desktop of the last ~15 years).
- On Linux, the OpenGL/X11 and FFmpeg development packages:

  ```bash
  sudo apt-get install -y build-essential cmake ninja-build pkg-config \
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

```bash
cmake -S PS2Recomp -B PS2Recomp/out/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build PS2Recomp/out/build --target ps2_recomp -j"$(nproc)"
```

### 3. Translate the game code

Rewrites the game's code as ~33k C++ files under
`PS2Recomp/ps2xRuntime/src/runner/`:

```bash
./PS2Recomp/out/build/ps2xRecomp/ps2_recomp configs/jca_config.toml
```

### 4. Build the game

Compiles the generated files and links the final executable — this is the long
step; expect around half an hour on a recent machine:

```bash
cmake --build PS2Recomp/out/build --target ps2EntryRunner -j"$(nproc)"
```

### 5. Run it

The runner takes the extracted game program, not the ISO:

```bash
./PS2Recomp/out/build/ps2xRuntime/ps2EntryRunner game/SCES_524.12
```

On Windows the steps are the same, but pass `--config Release` to the build
commands, replace `-j"$(nproc)"` with a plain core count (e.g. `-j8`), and
look for the executables in `Release\` subdirectories of the paths above;
`PS2Recomp/.github/workflows/build.yml` shows the reference toolchain setup.

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

`ghidra_output` points at a function map produced by the Ghidra export
(`PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java`) during this
project's development. That map is not needed to build — without it the
recompiler classifies from the ELF's relocations alone and prints a warning.
You can delete the line, point it at your own export, or let the analyzer
build a config from scratch (build the analyzer target first):

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
| `configs/` | Function classification for the translator: which functions are library stubs, which are skipped, what to patch. |
| `tools/` | `extract_iso.py` — pulls the game executable out of your own disc image. |
| `game/` | Your disc image and the executable extracted from it (created by step 1). |
| `local/` | Analysis output, logs and captures created while working on the project. |

## Legal

This project is not affiliated with, endorsed by, or sponsored by Sony
Interactive Entertainment, Atomi Entertainment, Eutechnyx, or any other rights
holder. "Jackie Chan Adventures" and all related marks and game content belong
to their respective owners.

You need to own the game to build this. Nothing in this repository is derived
from the game's assets or executable, and nothing built from it may be
redistributed. See `NOTICE`.

The project code is released under GPL-3.0; the vendored PS2Recomp toolchain
and runtime are GPL-3.0 as well. See `LICENSE` and `PS2Recomp/LICENSE`.
