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
- **Python 3 MML Compiler** (`tools/mdxnt_compile.py`):
  - Compiles `.mml` to compact `.mdxt` binaries or C headers (`--header`).

## Quick Start

```sh
# Run compiler + host mock-OPN driver unit tests
make test

# Optional: verify Z80 SDCC build (requires sdcc / sdasz80)
make check-z80

# Compile MML to a C header
python3 tools/mdxnt_compile.py --header MY_BGM=bgm.mml MY_SE=se.mml -o out_mdxt.h
```

## Directory Layout
- `include/mdxnt.h` — Public API & HAL hook declarations
- `src/mdxnt_drv.c` — Portable C99 YM2203 OPN sound driver
- `hal/pc88_sr/mdxnt_hal_pc88.s` — PC-8801mkIISR Z80 SDCC HAL
- `tools/mdxnt_compile.py` — MDX-compatible MML compiler (`.mml` → `.mdxt` / `.h`)
- `tests/` — Compiler & host mock-OPN driver unit tests
