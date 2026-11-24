use std::path::Path;

/// Emit a C header containing named `static const uint8_t` arrays for `.mdxt` binaries.
pub fn emit_c_header(songs: &[(String, Vec<u8>)], out_path: &Path) -> String {
    let fname = out_path
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("OUT_MDXT_H");
    let guard: String = fname
        .to_ascii_uppercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect();

    let mut lines = vec![
        format!("#ifndef {guard}"),
        format!("#define {guard}"),
        "#include <stdint.h>".to_string(),
        String::new(),
    ];

    for (sym, data) in songs {
        lines.push(format!("static const uint8_t {sym}[{}] = {{", data.len()));
        for chunk in data.chunks(16) {
            let formatted: Vec<String> = chunk.iter().map(|b| format!("0x{b:02X}u")).collect();
            lines.push(format!("    {},", formatted.join(", ")));
        }
        lines.push("};".to_string());
        lines.push(String::new());
    }

    lines.push(format!("#endif /* {guard} */"));
    lines.join("\n") + "\n"
}

pub fn default_symbol_name(path: &Path) -> String {
    let stem = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("SONG");
    let clean: String = stem
        .to_ascii_uppercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect();
    format!("MDXNT_{clean}")
}
