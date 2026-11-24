use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process;

use mdxnt_core::{compile_mml, default_symbol_name, emit_c_header};

fn print_usage() {
    eprintln!("Usage:");
    eprintln!("  mdxnt <input.mml> -o <out.mdxt>");
    eprintln!("  mdxnt --header [SYM=]<file.mml>... -o <out.h>");
    eprintln!("  mdxnt --stdout <input.mml | ->");
    eprintln!("  mdxnt --check <input.mml | ->");
}

fn read_source(path: &str) -> io::Result<String> {
    if path == "-" {
        let mut buf = String::new();
        io::stdin().read_to_string(&mut buf)?;
        Ok(buf)
    } else {
        fs::read_to_string(path)
    }
}

fn compile_or_exit(label: &str, src: &str) -> Vec<u8> {
    match compile_mml(src) {
        Ok(data) => data,
        Err(diags) => {
            for d in diags {
                eprintln!("{label}:{d}");
            }
            process::exit(1);
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        print_usage();
        process::exit(2);
    }

    let mut header_mode = false;
    let mut stdout_mode = false;
    let mut check_mode = false;
    let mut out_path: Option<PathBuf> = None;
    let mut inputs: Vec<String> = Vec::new();

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-h" | "--help" => {
                print_usage();
                return;
            }
            "--header" => header_mode = true,
            "--stdout" => stdout_mode = true,
            "--check" => check_mode = true,
            "-o" | "--out" => {
                i += 1;
                if i >= args.len() {
                    eprintln!("error: missing argument for -o/--out");
                    process::exit(2);
                }
                out_path = Some(PathBuf::from(&args[i]));
            }
            other => inputs.push(other.to_string()),
        }
        i += 1;
    }

    if inputs.is_empty() {
        eprintln!("error: no input files specified");
        process::exit(2);
    }

    if check_mode {
        for path in &inputs {
            let src = read_source(path).unwrap_or_else(|e| {
                eprintln!("error: failed to read {path}: {e}");
                process::exit(1);
            });
            let _ = compile_or_exit(path, &src);
        }
        return;
    }

    if stdout_mode {
        let path = &inputs[0];
        let src = read_source(path).unwrap_or_else(|e| {
            eprintln!("error: failed to read {path}: {e}");
            process::exit(1);
        });
        let data = compile_or_exit(path, &src);
        let mut stdout = io::stdout().lock();
        if let Err(e) = stdout.write_all(&data) {
            eprintln!("error: failed to write stdout: {e}");
            process::exit(1);
        }
        return;
    }

    let Some(out) = out_path else {
        eprintln!("error: -o/--out is required");
        process::exit(2);
    };

    if let Some(parent) = out.parent() {
        if !parent.as_os_str().is_empty() {
            let _ = fs::create_dir_all(parent);
        }
    }

    if header_mode {
        let mut songs: Vec<(String, Vec<u8>)> = Vec::new();
        for spec in &inputs {
            let (sym, path) = if let Some((s, p)) = spec.split_once('=') {
                (s.to_string(), p.to_string())
            } else {
                (default_symbol_name(Path::new(spec)), spec.clone())
            };
            let src = read_source(&path).unwrap_or_else(|e| {
                eprintln!("error: failed to read {path}: {e}");
                process::exit(1);
            });
            let data = compile_or_exit(&path, &src);
            println!("  [mdxn] {:24} <- {} ({} B)", sym, path, data.len());
            songs.push((sym, data));
        }
        let total: usize = songs.iter().map(|(_, d)| d.len()).sum();
        let header = emit_c_header(&songs, &out);
        if let Err(e) = fs::write(&out, header) {
            eprintln!("error: failed to write {}: {e}", out.display());
            process::exit(1);
        }
        println!("Generated {} ({total} B total)", out.display());
    } else {
        let path = &inputs[0];
        let src = read_source(path).unwrap_or_else(|e| {
            eprintln!("error: failed to read {path}: {e}");
            process::exit(1);
        });
        let data = compile_or_exit(path, &src);
        if let Err(e) = fs::write(&out, &data) {
            eprintln!("error: failed to write {}: {e}", out.display());
            process::exit(1);
        }
        println!("Compiled {path} -> {} ({} B)", out.display(), data.len());
    }
}
