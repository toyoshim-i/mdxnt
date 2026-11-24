# mdxnt-rs — Rust MML Compiler & Language Server

Rust Cargo workspace providing a span-aware **mdxnt** MML compiler (byte-for-byte compatible with `tools/mdxnt_compile.py`) and an LSP server.

## Workspace Layout

- **`crates/mdxnt-core`**: Core MML lexer/parser, `.mdxt` bytecode compiler, 38-int/47-int FM voice packer, C header emitter, UTF-16 diagnostics, and semantic token generator.
- **`crates/mdxnt-cli`** (`mdxnt` binary): Command-line compiler compatible with `tools/mdxnt_compile.py`.
- **`crates/mdxnt-lsp`** (`mdxnt-lsp` binary): Language Server Protocol server over `stdio` using `tower-lsp`.

## Build & Test

```sh
# Run unit tests
cargo test --manifest-path tools/mdxnt-rs/Cargo.toml

# Build release binaries (mdxnt and mdxnt-lsp)
cargo build --release --manifest-path tools/mdxnt-rs/Cargo.toml
```

Binaries are placed in `tools/mdxnt-rs/target/release/{mdxnt,mdxnt-lsp}`.

## `mdxnt` CLI Usage

```sh
# Compile .mml to .mdxt binary
mdxnt song.mml -o song.mdxt

# Compile multiple .mml files into a C header
mdxnt --header MY_BGM=bgm.mml MY_SE=se.mml -o out_mdxt.h

# Syntax/semantic check only (exit 0 if valid, 1 with line:col diagnostics on error)
mdxnt --check song.mml

# Compile from stdin (or file) and write raw .mdxt bytes to stdout
mdxnt --stdout - < song.mml > song.mdxt
```

## `mdxnt-lsp` Language Server

`mdxnt-lsp` communicates over standard input/output (JSON-RPC / LSP) and can be used with the VS Code extension in `tools/code/` or any LSP-capable editor (Neovim, Helix, Zed, Emacs, etc.).

Supported LSP capabilities:
- `textDocument/publishDiagnostics`: Live syntax, voice-range, repeat-depth, and 16-bit offset diagnostics with document versioning.
- `textDocument/semanticTokens/full`: Non-overlapping semantic tokens for comments, directives, channels, voice definitions, notes, rests, octaves, lengths, tempo, repeats, loops, voice selects, volume/gate/tie, and modulation/mixer commands.
- `textDocument/hover`: Context-aware Markdown docs for commands, FM voice summaries (`FB`/`CON`/format/definition line), and built-in SSG envelopes (`@0`–`@5`).
- `textDocument/completion`: Completions triggered on `@` and `#` (including all FM voices defined in the current document).
