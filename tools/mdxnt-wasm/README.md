# mdxnt-wasm — WebAssembly YM2203 (OPN) Player Engine

Compiles the real portable C99 sound driver (`src/mdxnt_drv.c` with `-DMDXNT_MOCK_HAL`) together with `third_party/ymfm` (`ymfm::ym2203`, BSD-3-Clause) into a compact (~58 KB) standalone WebAssembly reactor module (`tools/code/media/mdxnt_player.wasm`).

## Architecture

- **`opn_chip.h`**: Minimal abstract `OpnChip` interface (`reset`, `write`, `next_native_sample`, `native_sample_rate`), allowing the synthesis backend to be swapped (e.g., to a Nuked backend) without changing the player or driver glue.
- **`ymfm_chip.cpp`**: `ymfm::ym2203` implementation clocked at the PC-8801mkIISR master clock (`3.9936 MHz`, `OPN_FIDELITY_MIN` = `166.4 kHz`), with a 1-pole DC-blocking HPF on the unipolar SSG output.
- **`mdxnt_player.cpp`**: Implements the `mdxnt` HAL hooks, boxcar integrating resampler to the host WebAudio sample rate, variable-speed Timer-B scheduling, silent tick seeking, and per-channel FM (key-off + TL=127 shadow) / SSG (volume shadow) muting.
- **`test_host.cpp`**: Native C++ test harness verifying PCM rendering, 4x fast-forward, and seeking on host.

## Build Requirements

- **`wasi-sdk`** (Clang/LLVM for `wasm32-unknown-wasi`, default path `~/.local/wasi-sdk`; override with `WASI_SDK=/path/to/wasi-sdk`).
- `third_party/ymfm` git submodule initialized (`git submodule update --init --recursive`).

```sh
# Build tools/code/media/mdxnt_player.wasm
make -C tools/mdxnt-wasm wasm

# Run native host verification test (requires build/demo_rs.mdxt)
make -C tools/mdxnt-wasm host-test
```

## Exported Wasm C ABI

| Export | Signature | Description |
|---|---|---|
| `_initialize` | `() -> void` | Wasm reactor initializer (call once after instantiation). |
| `mdxnt_wasm_init` | `(sample_rate: u32) -> void` | Initialize YM2203 chip and driver for target output sample rate (`8000..192000` Hz). |
| `mdxnt_wasm_get_data_buf` | `() -> *mut u8` | Pointer to the internal 64 KB `.mdxt` input buffer in Wasm linear memory. |
| `mdxnt_wasm_get_data_capacity` | `() -> u32` | Capacity of the `.mdxt` input buffer (`65536` bytes). |
| `mdxnt_wasm_load` | `(len: u32) -> u32` | Validate and start playback of `len` bytes written to the data buffer (`1` = ok, `0` = invalid). |
| `mdxnt_wasm_stop` | `() -> void` | Stop playback and reset chip/audio state. |
| `mdxnt_wasm_render` | `(frames: u32) -> u32` | Render up to `4096` stereo interleaved `f32` frames into the PCM buffer; returns frames rendered. |
| `mdxnt_wasm_get_pcm_buf` | `() -> *const f32` | Pointer to the stereo interleaved `f32` PCM output buffer (`4096 * 2` floats). |
| `mdxnt_wasm_set_speed` | `(speed_percent: u32) -> void` | Set playback speed in percent (`25..1600`, e.g. `100` = 1x, `400` = 4x). |
| `mdxnt_wasm_seek_ticks` | `(target_ticks: u32) -> u32` | Silently advance the driver from tick 0 to `target_ticks` (preserving playing/stopped state); returns reached tick. |
| `mdxnt_wasm_set_mute_mask` | `(mask: u32) -> void` | 6-bit mute mask (`bit 0..2` = FM 1–3, `bit 3..5` = SSG A–C). |
| `mdxnt_wasm_get_tick` | `() -> u32` | Current elapsed Timer-B tick count. |
| `mdxnt_wasm_get_tb` | `() -> u32` | Current YM2203 Timer-B register value (`0x26`). |
| `mdxnt_wasm_is_playing` | `() -> u32` | `1` while BGM is active, `0` when stopped or a non-looping track finishes. |
