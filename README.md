# mdxnt

Clean-room, MIT-licensed **MDX-compatible YM2203 (OPN: FM 3ch + SSG 3ch)** MML compiler and portable C99 sound driver, with a reference Z80 assembly HAL for **NEC PC-8801mkIISR**.

## Features
- **Clean-room MIT license**: Safe for both open-source and commercial retro/homebrew releases.
- **Portable C99 core** (`src/mdxnt_drv.c`, `include/mdxnt.h`):
  - 6-channel BGM (FM ch 1–3 + SSG ch A–C) + 2-channel priority SE hijacking (SSG ch B–C) with automatic BGM restoration.
  - Contention-free one-shot SSG voice blips (`mdxnt_ssg_blip`) serviced inside the Timer-B ISR.
  - 4-op FM voices (38-int MUCOM/OPN or 47-int MDX `@N = { ... }` format), logarithmic 16-step FM volume (`FM_VOL_TL`), and accurate YM2203 `CON 0..7` carrier masks.
  - 6 built-in SSG ADSR software envelopes (`@0..@5`) with smooth Release tails, noise mixer (`P0..2`, `w0..31`), detune (`D`), pitch sweep (`_`), software pitch LFO (`M delay,speed,depth,count`), nested repeats (`[ / ]N` up to 4 levels), ties (`&`, `^`), and ratio (`q1..8`) / fixed-tail (`@qN`) gate times.
  - Zero 16-bit software multiply/divide in the Timer-B ISR hot path (4 MHz Z80 friendly).
- **Thin PC-8801mkIISR Z80 HAL** (`hal/pc88_sr/mdxnt_hal_pc88.s`, ~110 bytes):
  - YM2203 port `0x44`/`0x45` busy-polling, Timer-B IRQ (`INT 4`) ack/setup, and Port `0x32`/`0xE4` shadow preservation.
- **MML Compilers & Language Server**:
  - **Rust workspace** (`tools/mdxnt-rs/`): byte-for-byte compatible `mdxnt` CLI compiler + `mdxnt-lsp` Language Server (live diagnostics, semantic highlighting, hover docs, completions).
  - **Python 3 compiler** (`tools/mdxnt_compile.py`): zero-dependency fallback compiling `.mml` to `.mdxt` binaries or C headers (`--header`).
- **VS Code Extension & Wasm OPN Player** (`tools/code/`, `tools/mdxnt-wasm/`):
  - One-click compile & play, fast-forward (`0.5x`–`8x`), bar skip/seek, and 6-channel mute/solo powered by the real `src/mdxnt_drv.c` driver + `third_party/ymfm` (`ym2203`) running in WebAssembly (`AudioWorklet`).

## Quick Start

```sh
# Clone with submodules (third_party/ymfm)
git clone --recursive https://github.com/toyoshim-i/mdxnt.git

# Run Python compiler + host mock-OPN driver + Rust compiler tests
make test

# Build Rust CLI/LSP, Wasm OPN player (requires wasi-sdk), and VS Code extension
make tools

# Optional: verify Z80 SDCC build (requires sdcc / sdasz80)
make check-z80

# Compile MML to a C header (Rust CLI or Python)
tools/mdxnt-rs/target/release/mdxnt --header MY_BGM=examples/demo.mml -o out_mdxt.h
python3 tools/mdxnt_compile.py --header MY_BGM=examples/demo.mml -o out_mdxt.h
```

## Directory Layout
- `include/mdxnt.h` — Public API & HAL hook declarations
- `src/mdxnt_drv.c` — Portable C99 YM2203 OPN sound driver
- `hal/pc88_sr/mdxnt_hal_pc88.s` — PC-8801mkIISR Z80 SDCC HAL
- `examples/demo.mml` — 6-channel (FM 3ch + SSG 3ch) demo track
- `tools/mdxnt_compile.py` — Python 3 MDX-compatible MML compiler
- `tools/mdxnt-rs/` — Rust MML compiler & LSP ([README](tools/mdxnt-rs/README.md))
- `tools/mdxnt-wasm/` — WebAssembly YM2203 OPN player engine ([README](tools/mdxnt-wasm/README.md))
- `tools/code/` — VS Code extension with LSP & WebAudio player ([README](tools/code/README.md))
- `third_party/ymfm/` — `aaronsgiles/ymfm` submodule (BSD-3-Clause, used only by the Wasm preview player)
- `tests/` — Compiler & host mock-OPN driver unit tests
