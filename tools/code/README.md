# mdxnt MML — VS Code Extension

VS Code language support and instant **YM2203 (OPN: FM 3ch + SSG 3ch)** WebAudio + WebAssembly player for **mdxnt** `.mml` files.

## Features

- **Syntax Highlighting**: Instant TextMate highlighting plus semantic tokens from `mdxnt-lsp`.
- **Language Server (`mdxnt-lsp`)**:
  - Real-time syntax & range diagnostics (FM voice bounds, SSG envelope bounds, repeat nesting, 16-bit offset limits, etc.).
  - Hover documentation for MML commands, built-in SSG envelopes (`@0`–`@5`), and defined FM voices (`@N` showing FB/CON and definition line).
  - Autocompletion for directives (`#tempo`), commands, and defined `@N` FM voices.
- **One-Click Compile & Play**:
  - Compiles the active `.mml` editor buffer in memory (no need to save first) and plays it immediately using the real `src/mdxnt_drv.c` driver + `ymfm` YM2203 emulator in WebAssembly (`AudioWorklet`).
- **Interactive OPN Player**:
  - Play / Pause / Stop, ±1 Bar skip, interactive seek bar, and live Tick / Bar:Beat / BPM / Level Meter display.
  - Variable playback speed (`0.5x`, `1x`, `2x`, `4x`, `8x`).
  - Per-channel **Mute** (Click) and **Solo** (`Shift+Click`) for FM 1–3 (`A`–`C`) and SSG A–C (`D`–`F`).
  - Keeps playing in the background when switching editor tabs (`retainContextWhenHidden`).

## Prerequisites & Build

From the repository root:

```sh
# Ensure third_party/ymfm submodule is initialized
git submodule update --init --recursive

# Build mdxnt / mdxnt-lsp (Rust), mdxnt_player.wasm (wasi-sdk), and the VS Code extension
make tools
```

Or build only the extension TypeScript code:

```sh
cd tools/code
npm install
npm run compile
```

## Running in VS Code

Launch VS Code with the extension loaded in development mode:

```sh
code --extensionDevelopmentPath=/path/to/mdxnt/tools/code /path/to/mdxnt
```

Open `examples/demo.mml` and click the **▶** button in the top-right editor toolbar (or press `Ctrl+Alt+P` / `Cmd+Alt+P`).

> **Note on WebAudio Autoplay**: On the very first playback after opening the player panel, if Chromium suspends the `AudioContext` until a gesture occurs inside the Webview, click **▶ Play** once inside the player panel to unlock audio.

## Controls & Shortcuts

| Action | Toolbar Button | Shortcut (Linux/Win) | Shortcut (macOS) | Command ID |
|---|---|---|---|---|
| Compile & Play | `▶` (editor title) | `Ctrl+Alt+P` | `Cmd+Alt+P` | `mdxnt.compileAndPlay` |
| Toggle Fast Forward (4x) | `▷` (editor title) | `Ctrl+Alt+F` | `Cmd+Alt+F` | `mdxnt.fastForward` |
| Stop Playback | `■` (editor title) | `Ctrl+Alt+S` | `Cmd+Alt+S` | `mdxnt.stop` |
| Compile to `.mdxt` file | — | Command Palette | Command Palette | `mdxnt.compile` |
| Open OPN Player Panel | — | Command Palette | Command Palette | `mdxnt.openPlayer` |

### Player Panel Controls
- **▶ Play / ⏸ Pause**: Toggle playback (also unlocks WebAudio on first click if suspended).
- **■ Stop**: Stop playback and reset to tick 0.
- **⏪ -1 Bar / ⏩ +1 Bar**: Jump backward or forward by 192 ticks (1 bar in 4/4).
- **Seek Slider**: Drag to seek silently to any tick position.
- **Playback Speed (`0.5x` – `8x`)**: Change playback tempo multiplier in real time.
- **Channel Buttons (`FM 1–3`, `SSG A–C`)**:
  - **Click**: Toggle mute for that channel.
  - **Shift + Click**: Toggle solo for that channel.

## Settings

| Setting | Default | Description |
|---|---|---|
| `mdxnt.compilerPath` | `""` | Path (or command name on `PATH`) to the `mdxnt` Rust CLI binary. |
| `mdxnt.lspPath` | `""` | Path (or command name on `PATH`) to the `mdxnt-lsp` Language Server binary. |

When left empty, the extension auto-detects binaries in this order:
1. `<repo>/tools/mdxnt-rs/target/release/`
2. `<repo>/tools/mdxnt-rs/target/debug/`
3. `<workspace>/tools/mdxnt-rs/target/{release,debug}/`
4. System `PATH`

## MML Quick Reference

| Syntax | Meaning |
|---|---|
| `; comment` | Line comment |
| `#tempo <30..300>` | Initial song tempo in BPM |
| `@N = { ... }` | FM voice definition (38-int MUCOM/OPN or 47-int MDX format, `N = 0..255`) |
| `A`–`C <mml>` | FM channels 1–3 (combinable, e.g. `AB c4 d4`) |
| `D`–`F <mml>` | SSG channels A–C |
| `c d e f g a b` (`+`/`#`/`-`) | Notes with optional length (`4`, `8.`, `%12`) and `^` tie extension |
| `r` | Rest with optional length and `^` extension |
| `o<0..7>`, `<`, `>` | Octave set / down / up |
| `l<len>` | Default note/rest length |
| `t<BPM>`, `@t<0..255>` | Tempo in BPM or raw YM2203 Timer-B value |
| `@<N>` | Select FM voice (ch `A`–`C`) or built-in SSG ADSR envelope `@0`–`@5` (ch `D`–`F`) |
| `v<0..15>`, `(`, `)` | Volume set / -1 / +1 |
| `q<1..8>`, `@q<0..127>` | Gate time ratio (`q8` = full) or fixed key-off tail ticks |
| `&` | Tie / slur into next note |
| `[ ... / ... ]N` | Repeat `N` times (up to 4 nested levels; `/` escapes on final pass) |
| `L` | Infinite loop point for the channel |
| `D[+-]N`, `_[+-]N` | Detune / per-tick pitch sweep |
| `M delay,speed,depth,count` | Software pitch LFO (`M0` to disable) |
| `P<0..2>`, `w<0..31>` | SSG mixer (`0`=Tone, `1`=Noise, `2`=Both) and noise period |
