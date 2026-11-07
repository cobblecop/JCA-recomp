# Jackie Chan Adventures — PS2 static recompilation

A static recompilation of the PAL PlayStation 2 release of *Jackie Chan
Adventures* (`SCES-52412`, Europe/Australia, En/Fr/De/Es/It/Nl/Pt), built on
[PS2Recomp](https://github.com/ran-j/PS2Recomp). The game's MIPS R5900 code is
translated to C++ once and then compiled natively — no emulator, no
interpretation at runtime.

**No game code, disc image or game assets are distributed here.** You bring
your own copy of the game; the translated C++ is generated on your machine from
your own disc and stays out of version control.

## Status

It boots and renders, controller input works, and the language-select and
memory-card screens behave. Bringing up the rest of the game is ongoing.

## Layout

| Path | Contents |
| --- | --- |
| `PS2Recomp/` | Vendored PS2Recomp toolchain and runtime (GPL-3.0, upstream history in `PS2Recomp/README.md`), plus this project's runtime fixes. |
| `configs/` | Function classification for the translator: which functions are library stubs, which are skipped, what to patch. |
| `tools/` | `extract_iso.py` — pulls the game executable out of your own disc image. |
| `game/` | Your disc image and the executable extracted from it. **Not tracked.** |
| `local/` | Local analysis output, logs, captures. **Not tracked.** |

## Requirements

- Your own PAL copy of the game (`SCES-52412`).
- CMake 3.20+ and a C++20 compiler (GCC or Clang on Linux, MSVC on Windows).
- SDL3 and OpenGL development packages — see `PS2Recomp/README.md`.
- Python 3 (only for the disc extraction helper).

## Build

```bash
# 1. Extract the executable from your own disc image.
python3 tools/extract_iso.py "/path/to/Jackie Chan Adventures.iso"
#    -> game/SCES_524.12

# 2. Build the translator.
cmake -S PS2Recomp -B PS2Recomp/out/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build PS2Recomp/out/build --target ps2_recomp -j"$(nproc)"

# 3. Translate the game code. Output lands in
#    PS2Recomp/ps2xRuntime/src/runner/ (ignored by git).
./PS2Recomp/out/build/ps2xRecomp/ps2_recomp configs/jca_config.toml

# 4. Build the runtime plus the freshly generated code.
cmake --build PS2Recomp/out/build --target ps2EntryRunner -j"$(nproc)"

# 5. Run (the runner takes the executable, not the ISO).
cd PS2Recomp/out/build/ps2xRuntime
./ps2EntryRunner ../../../../game/SCES_524.12
```

### About `configs/jca_config.toml`

Paths in the config are relative to the repository root, so run the tools from
there, as in the steps above. `input` is your extracted `SCES_524.12` and
`output` is where the translated C++ is written; neither should need editing.

`ghidra_output` points at a function map produced by the Ghidra export
(`PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java`) during this
project's development. That map is a local artifact and is not part of this
repository — without it the recompiler classifies from the ELF's relocations
alone and prints a warning. You can delete the line, point it at your own
export, or let the analyzer build a config from scratch:

```bash
./PS2Recomp/out/build/ps2xAnalyzer/ps2_analyzer game/SCES_524.12 configs/jca_config.toml
```

## Debug output

A normal run stays quiet; the bring-up diagnostics are opt-in:

```bash
PS2_TRACE=1 ./ps2EntryRunner ../../../../game/SCES_524.12
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

## Legal

This project is not affiliated with, endorsed by, or sponsored by Sony
Interactive Entertainment, Atomi Entertainment, Eutechnyx, or any other rights
holder. "Jackie Chan Adventures" and all related marks and game content belong
to their respective owners.

You need to own the game to build this. Nothing in this repository is derived
from the game's assets or executable, and nothing built from it may be
redistributed. See `NOTICE`.

## Licence

GPL-3.0. The vendored PS2Recomp toolchain and runtime are GPL-3.0 as well; see
`LICENSE` and `PS2Recomp/LICENSE`.
