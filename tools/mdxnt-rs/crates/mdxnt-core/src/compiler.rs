use std::collections::BTreeMap;

use crate::diagnostic::{Diagnostic, SemanticToken, Span, TokenKind};
use crate::voice::{pack_opn_voice, VoiceInfo, DEFAULT_VOICE};

pub const TPQ: i32 = 48;
pub const WHOLE_TICKS: i32 = TPQ * 4;
pub const MAX_REP_DEPTH: usize = 4;
pub const MAX_VOICE_ID: usize = 255;
pub const MAX_NOTE_TICKS: i32 = 65536;

pub fn bpm_to_timer_b(bpm: i32) -> u8 {
    let bpm = bpm.clamp(30, 300) as f64;
    let tb = (256.0 - 4333.333333333333 / bpm).round_ties_even() as i32;
    tb.clamp(0, 255) as u8
}

#[derive(Debug, Clone)]
pub struct AnalysisResult {
    pub binary: Option<Vec<u8>>,
    pub diagnostics: Vec<Diagnostic>,
    pub tokens: Vec<SemanticToken>,
    pub voices: BTreeMap<usize, VoiceInfo>,
    pub channel_ticks: [u32; 6],
    pub init_tb: u8,
}

#[derive(Debug, Clone, Copy)]
struct SrcChar {
    ch: char,
    line: u32,
    col: u32,
}

fn note_semitone(c: char) -> Option<i32> {
    match c {
        'c' => Some(0),
        'd' => Some(2),
        'e' => Some(4),
        'f' => Some(5),
        'g' => Some(7),
        'a' => Some(9),
        'b' => Some(11),
        _ => None,
    }
}

fn parse_uint(chars: &[SrcChar], mut pos: usize) -> Option<(i32, usize)> {
    let start = pos;
    let mut val: i32 = 0;
    while pos < chars.len() && chars[pos].ch.is_ascii_digit() {
        val = val.saturating_mul(10).saturating_add((chars[pos].ch as u8 - b'0') as i32);
        pos += 1;
    }
    if pos == start {
        None
    } else {
        Some((val, pos))
    }
}

fn parse_int(chars: &[SrcChar], mut pos: usize) -> Option<(i32, usize)> {
    let start = pos;
    let mut neg = false;
    if pos < chars.len() && (chars[pos].ch == '+' || chars[pos].ch == '-') {
        neg = chars[pos].ch == '-';
        pos += 1;
    }
    let digit_start = pos;
    let mut val: i32 = 0;
    while pos < chars.len() && chars[pos].ch.is_ascii_digit() {
        val = val.saturating_mul(10).saturating_add((chars[pos].ch as u8 - b'0') as i32);
        pos += 1;
    }
    if pos == digit_start {
        let _ = start;
        None
    } else {
        Some((if neg { -val } else { val }, pos))
    }
}

fn span_of(chars: &[SrcChar], start: usize, end_excl: usize) -> Span {
    if chars.is_empty() {
        return Span::single(0, 0);
    }
    let s = start.min(chars.len() - 1);
    let e = end_excl.saturating_sub(1).max(s).min(chars.len() - 1);
    Span {
        start_line: chars[s].line,
        start_col: chars[s].col,
        end_line: chars[e].line,
        end_col: chars[e].col + chars[e].ch.len_utf16() as u32,
    }
}

fn emit_token_range(
    tokens: &mut Vec<SemanticToken>,
    chars: &[SrcChar],
    start: usize,
    end_excl: usize,
    kind: TokenKind,
) {
    if start >= end_excl || start >= chars.len() {
        return;
    }
    let end = end_excl.min(chars.len());
    let mut seg_start = start;
    while seg_start < end {
        let line = chars[seg_start].line;
        let col0 = chars[seg_start].col;
        let mut seg_end = seg_start + 1;
        let mut utf16_len = chars[seg_start].ch.len_utf16() as u32;
        while seg_end < end
            && chars[seg_end].line == line
            && chars[seg_end].col == col0 + utf16_len
        {
            utf16_len += chars[seg_end].ch.len_utf16() as u32;
            seg_end += 1;
        }
        tokens.push(SemanticToken {
            line,
            start_col: col0,
            length: utf16_len,
            kind,
        });
        seg_start = seg_end;
    }
}

fn parse_length(
    chars: &[SrcChar],
    mut pos: usize,
    default_ticks: i32,
) -> Result<(i32, usize), Diagnostic> {
    let n = chars.len();
    if pos < n && chars[pos].ch == '%' {
        let pct_pos = pos;
        pos += 1;
        let Some((val, next_pos)) = parse_uint(chars, pos) else {
            return Err(Diagnostic::error(
                span_of(chars, pct_pos, pos),
                "Expected tick count after '%'",
            ));
        };
        return Ok((val.max(1), next_pos));
    }
    let base = if let Some((val, next_pos)) = parse_uint(chars, pos) {
        pos = next_pos;
        if val > 0 {
            WHOLE_TICKS / val
        } else {
            default_ticks
        }
    } else {
        default_ticks
    };
    let mut ticks = base;
    let mut add = base;
    while pos < n && chars[pos].ch == '.' {
        add /= 2;
        ticks += add;
        pos += 1;
    }
    Ok((ticks.max(1), pos))
}

struct RepeatFrame {
    start_pos: usize,
    start_src_idx: usize,
    escapes: Vec<usize>,
    start_ticks: u32,
}

fn compile_channel(
    chars: &[SrcChar],
    ch_idx: usize,
    base_off: usize,
    max_v: usize,
    diagnostics: &mut Vec<Diagnostic>,
    tokens: &mut Vec<SemanticToken>,
) -> (Vec<u8>, u32) {
    let mut out: Vec<u8> = Vec::new();
    let mut octave: i32 = 4;
    let mut def_ticks: i32 = TPQ;
    let mut loop_abs_off: u16 = 0;
    let mut loop_src_idx: usize = 0;
    let mut loop_ticks: u32 = 0;
    let mut total_ticks: u32 = 0;
    let mut rep_stack: Vec<RepeatFrame> = Vec::new();
    let mut ssg_mode: u8 = 0;
    let mut ssg_nfreq: u8 = 12;

    let mut i = 0usize;
    let n = chars.len();

    while i < n {
        let c = chars[i].ch;
        if matches!(c, ' ' | '\t' | '\r' | '\n' | '|' | ',') {
            i += 1;
            continue;
        }

        // Notes c..b
        if let Some(mut semi) = note_semitone(c) {
            let tok_start = i;
            i += 1;
            while i < n && matches!(chars[i].ch, '+' | '#' | '-') {
                if chars[i].ch == '-' {
                    semi -= 1;
                } else {
                    semi += 1;
                }
                i += 1;
            }
            let note = (octave * 12 + semi).clamp(0, 95) as u8;
            let (mut ticks, next_i) = match parse_length(chars, i, def_ticks) {
                Ok(v) => v,
                Err(d) => {
                    diagnostics.push(d);
                    i += 1;
                    continue;
                }
            };
            i = next_i;
            while i < n && chars[i].ch == '^' {
                i += 1;
                match parse_length(chars, i, def_ticks) {
                    Ok((t2, next_i2)) => {
                        ticks = ticks.saturating_add(t2);
                        i = next_i2;
                    }
                    Err(d) => {
                        diagnostics.push(d);
                        break;
                    }
                }
            }
            emit_token_range(tokens, chars, tok_start, i, TokenKind::Note);
            if ticks > MAX_NOTE_TICKS {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    format!("Note duration {ticks} ticks exceeds maximum ({MAX_NOTE_TICKS})"),
                ));
                continue;
            }
            while ticks > 256 {
                out.push(0x80 + note);
                out.push(255);
                out.push(0xF7);
                ticks -= 256;
                total_ticks = total_ticks.saturating_add(256);
                if loop_abs_off != 0 {
                    loop_ticks = loop_ticks.saturating_add(256);
                }
            }
            out.push(0x80 + note);
            out.push((ticks - 1) as u8);
            total_ticks = total_ticks.saturating_add(ticks as u32);
            if loop_abs_off != 0 {
                loop_ticks = loop_ticks.saturating_add(ticks as u32);
            }
            continue;
        }

        // Rest r
        if c == 'r' {
            let tok_start = i;
            i += 1;
            let (mut ticks, next_i) = match parse_length(chars, i, def_ticks) {
                Ok(v) => v,
                Err(d) => {
                    diagnostics.push(d);
                    i += 1;
                    continue;
                }
            };
            i = next_i;
            while i < n && chars[i].ch == '^' {
                i += 1;
                match parse_length(chars, i, def_ticks) {
                    Ok((t2, next_i2)) => {
                        ticks = ticks.saturating_add(t2);
                        i = next_i2;
                    }
                    Err(d) => {
                        diagnostics.push(d);
                        break;
                    }
                }
            }
            emit_token_range(tokens, chars, tok_start, i, TokenKind::Rest);
            if ticks > MAX_NOTE_TICKS {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    format!("Rest duration {ticks} ticks exceeds maximum ({MAX_NOTE_TICKS})"),
                ));
                continue;
            }
            while ticks > 128 {
                out.push(127);
                ticks -= 128;
                total_ticks = total_ticks.saturating_add(128);
                if loop_abs_off != 0 {
                    loop_ticks = loop_ticks.saturating_add(128);
                }
            }
            out.push((ticks - 1) as u8);
            total_ticks = total_ticks.saturating_add(ticks as u32);
            if loop_abs_off != 0 {
                loop_ticks = loop_ticks.saturating_add(ticks as u32);
            }
            continue;
        }

        // Octave oN, <, >
        if c == 'o' {
            let tok_start = i;
            i += 1;
            if let Some((val, next_i)) = parse_uint(chars, i) {
                octave = val.clamp(0, 7);
                i = next_i;
                emit_token_range(tokens, chars, tok_start, i, TokenKind::Octave);
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Expected octave number after 'o'",
                ));
            }
            continue;
        }
        if c == '>' {
            octave = (octave + 1).min(7);
            emit_token_range(tokens, chars, i, i + 1, TokenKind::Octave);
            i += 1;
            continue;
        }
        if c == '<' {
            octave = (octave - 1).max(0);
            emit_token_range(tokens, chars, i, i + 1, TokenKind::Octave);
            i += 1;
            continue;
        }

        // Default length lN
        if c == 'l' {
            let tok_start = i;
            i += 1;
            match parse_length(chars, i, def_ticks) {
                Ok((val, next_i)) => {
                    def_ticks = val;
                    i = next_i;
                    emit_token_range(tokens, chars, tok_start, i, TokenKind::Length);
                }
                Err(d) => diagnostics.push(d),
            }
            continue;
        }

        // Tempo tBPM
        if c == 't' {
            let tok_start = i;
            i += 1;
            if let Some((bpm, next_i)) = parse_uint(chars, i) {
                out.push(0xFF);
                out.push(bpm_to_timer_b(bpm));
                i = next_i;
                emit_token_range(tokens, chars, tok_start, i, TokenKind::Tempo);
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Expected BPM after 't'",
                ));
            }
            continue;
        }

        // @t, @q, @N
        if c == '@' {
            let tok_start = i;
            i += 1;
            if i < n && chars[i].ch == 't' {
                i += 1;
                if let Some((tb, next_i)) = parse_uint(chars, i) {
                    out.push(0xFF);
                    out.push((tb & 0xFF) as u8);
                    i = next_i;
                    emit_token_range(tokens, chars, tok_start, i, TokenKind::Tempo);
                } else {
                    diagnostics.push(Diagnostic::error(
                        span_of(chars, tok_start, i),
                        format!("Expected Timer-B value after '@t' in ch {ch_idx}"),
                    ));
                }
            } else if i < n && chars[i].ch == 'q' {
                i += 1;
                let qn = if let Some((val, next_i)) = parse_uint(chars, i) {
                    i = next_i;
                    val.clamp(0, 127) as u8
                } else {
                    0
                };
                out.push(0xF8);
                out.push(0x80 | qn);
                emit_token_range(tokens, chars, tok_start, i, TokenKind::Gate);
            } else if let Some((vnum, next_i)) = parse_uint(chars, i) {
                i = next_i;
                let v = vnum as usize;
                if ch_idx < 3 && v >= max_v {
                    diagnostics.push(Diagnostic::error(
                        span_of(chars, tok_start, i),
                        format!(
                            "FM voice @{v} out of range (defined voices: {max_v}) in ch {ch_idx}"
                        ),
                    ));
                } else if ch_idx >= 3 && v >= 6 {
                    diagnostics.push(Diagnostic::error(
                        span_of(chars, tok_start, i),
                        format!("SSG envelope @{v} out of range (valid: 0..5) in ch {ch_idx}"),
                    ));
                } else {
                    out.push(0xFD);
                    out.push((vnum & 0xFF) as u8);
                }
                emit_token_range(tokens, chars, tok_start, i, TokenKind::VoiceSelect);
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Expected voice number after '@'",
                ));
            }
            continue;
        }

        // Volume vN, (, )
        if c == 'v' {
            let tok_start = i;
            i += 1;
            if let Some((vol, next_i)) = parse_uint(chars, i) {
                out.push(0xFB);
                out.push(vol.clamp(0, 15) as u8);
                i = next_i;
                emit_token_range(tokens, chars, tok_start, i, TokenKind::Volume);
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Expected volume after 'v'",
                ));
            }
            continue;
        }
        if c == '(' {
            out.push(0xFA);
            emit_token_range(tokens, chars, i, i + 1, TokenKind::Volume);
            i += 1;
            continue;
        }
        if c == ')' {
            out.push(0xF9);
            emit_token_range(tokens, chars, i, i + 1, TokenKind::Volume);
            i += 1;
            continue;
        }

        // Gate time q1..8
        if c == 'q' {
            let tok_start = i;
            i += 1;
            if let Some((qv, next_i)) = parse_uint(chars, i) {
                out.push(0xF8);
                out.push(qv.clamp(1, 8) as u8);
                i = next_i;
                emit_token_range(tokens, chars, tok_start, i, TokenKind::Gate);
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Expected gate after 'q'",
                ));
            }
            continue;
        }

        // Tie / Slur &
        if c == '&' {
            out.push(0xF7);
            emit_token_range(tokens, chars, i, i + 1, TokenKind::Tie);
            i += 1;
            continue;
        }

        // Loop point L
        if c == 'L' {
            if !rep_stack.is_empty() {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, i, i + 1),
                    format!("Loop point 'L' is not allowed inside '[...]' repeat in ch {ch_idx}"),
                ));
            } else {
                let abs_off = base_off + out.len();
                match u16::try_from(abs_off) {
                    Ok(off16) => {
                        loop_abs_off = off16;
                        loop_src_idx = i;
                        loop_ticks = 0;
                    }
                    Err(_) => {
                        diagnostics.push(Diagnostic::error(
                            span_of(chars, i, i + 1),
                            format!("Loop offset {abs_off} exceeds 16-bit .mdxt limit (65535 B)"),
                        ));
                    }
                }
            }
            emit_token_range(tokens, chars, i, i + 1, TokenKind::Loop);
            i += 1;
            continue;
        }

        // Repeat [ ... / ... ]N
        if c == '[' {
            if rep_stack.len() >= MAX_REP_DEPTH {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, i, i + 1),
                    format!("Repeat nesting exceeds max depth {MAX_REP_DEPTH} in ch {ch_idx}"),
                ));
            } else {
                let start_pos = out.len();
                out.extend_from_slice(&[0xF6, 2, 0x00]);
                rep_stack.push(RepeatFrame {
                    start_pos,
                    start_src_idx: i,
                    escapes: Vec::new(),
                    start_ticks: total_ticks,
                });
            }
            emit_token_range(tokens, chars, i, i + 1, TokenKind::Repeat);
            i += 1;
            continue;
        }
        if c == '/' {
            if let Some(frame) = rep_stack.last_mut() {
                let esc_pos = out.len();
                out.extend_from_slice(&[0xF4, 0, 0]);
                frame.escapes.push(esc_pos);
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, i, i + 1),
                    "Unmatched '/' outside repeat",
                ));
            }
            emit_token_range(tokens, chars, i, i + 1, TokenKind::Repeat);
            i += 1;
            continue;
        }
        if c == ']' {
            let tok_start = i;
            i += 1;
            let cnt = if let Some((val, next_i)) = parse_uint(chars, i) {
                i = next_i;
                val
            } else {
                2
            };
            emit_token_range(tokens, chars, tok_start, i, TokenKind::Repeat);
            if let Some(frame) = rep_stack.pop() {
                if cnt > 1 && total_ticks == frame.start_ticks {
                    diagnostics.push(Diagnostic::error(
                        span_of(chars, frame.start_src_idx, i),
                        format!("Zero-tick repeat loop '[...]{cnt}' in ch {ch_idx}"),
                    ));
                }
                out[frame.start_pos + 1] = cnt.clamp(1, 255) as u8;
                let end_cmd_pos = out.len();
                let rel_isize = (frame.start_pos + 3) as isize - (end_cmd_pos + 3) as isize;
                match i16::try_from(rel_isize) {
                    Ok(rel) => {
                        out.push(0xF5);
                        out.extend_from_slice(&rel.to_le_bytes());
                    }
                    Err(_) => {
                        diagnostics.push(Diagnostic::error(
                            span_of(chars, frame.start_src_idx, i),
                            "Repeat block size exceeds signed 16-bit relative jump limit",
                        ));
                    }
                }
                let after_end = out.len();
                for ep in frame.escapes {
                    let erel_isize = after_end as isize - (ep + 3) as isize;
                    match i16::try_from(erel_isize) {
                        Ok(erel) => {
                            let b = erel.to_le_bytes();
                            out[ep + 1] = b[0];
                            out[ep + 2] = b[1];
                        }
                        Err(_) => {
                            diagnostics.push(Diagnostic::error(
                                span_of(chars, frame.start_src_idx, i),
                                "Repeat escape '/' jump exceeds signed 16-bit limit",
                            ));
                        }
                    }
                }
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Unmatched ']'",
                ));
            }
            continue;
        }

        // Detune D[+-]N
        if c == 'D' {
            let tok_start = i;
            i += 1;
            if let Some((val, next_i)) = parse_int(chars, i) {
                i = next_i;
                emit_token_range(tokens, chars, tok_start, i, TokenKind::Modulation);
                match i16::try_from(val) {
                    Ok(v16) => {
                        out.push(0xF3);
                        out.extend_from_slice(&v16.to_le_bytes());
                    }
                    Err(_) => {
                        diagnostics.push(Diagnostic::error(
                            span_of(chars, tok_start, i),
                            format!("Detune value {val} out of signed 16-bit range (-32768..32767)"),
                        ));
                    }
                }
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Expected integer after 'D'",
                ));
            }
            continue;
        }

        // Pitch sweep _[+-]N
        if c == '_' {
            let tok_start = i;
            i += 1;
            if let Some((val, next_i)) = parse_int(chars, i) {
                i = next_i;
                emit_token_range(tokens, chars, tok_start, i, TokenKind::Modulation);
                match i16::try_from(val) {
                    Ok(v16) => {
                        out.push(0xF2);
                        out.extend_from_slice(&v16.to_le_bytes());
                    }
                    Err(_) => {
                        diagnostics.push(Diagnostic::error(
                            span_of(chars, tok_start, i),
                            format!("Pitch sweep value {val} out of signed 16-bit range (-32768..32767)"),
                        ));
                    }
                }
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Expected integer after '_'",
                ));
            }
            continue;
        }

        // Software Pitch LFO: M delay,speed,depth,count (or M0 / MF0 / MP...)
        if c == 'M' {
            let tok_start = i;
            i += 1;
            if i < n && matches!(chars[i].ch, 'F' | 'P') {
                i += 1;
            }
            let mut parts: Vec<i32> = Vec::new();
            if let Some((first, mut cur)) = parse_int(chars, i) {
                parts.push(first);
                loop {
                    let mut p = cur;
                    while p < n && matches!(chars[p].ch, ' ' | '\t') {
                        p += 1;
                    }
                    if p < n && chars[p].ch == ',' {
                        p += 1;
                        while p < n && matches!(chars[p].ch, ' ' | '\t') {
                            p += 1;
                        }
                        if let Some((val, next_cur)) = parse_int(chars, p) {
                            parts.push(val);
                            cur = next_cur;
                            continue;
                        }
                    }
                    break;
                }
                i = cur;
                emit_token_range(tokens, chars, tok_start, i, TokenKind::Modulation);
                if parts.len() == 1 && parts[0] == 0 {
                    out.extend_from_slice(&[0xEE, 0, 0, 0, 0, 0]);
                } else {
                    let delay = parts[0].clamp(0, 255) as u8;
                    let speed = parts.get(1).copied().unwrap_or(1).clamp(1, 255) as u8;
                    let depth = parts.get(2).copied().unwrap_or(0).clamp(-32768, 32767) as i16;
                    let mut cnt = parts.get(3).copied().unwrap_or(4).clamp(2, 254) as u8;
                    if (cnt & 1) != 0 {
                        cnt = (cnt + 1).min(254);
                    }
                    out.push(0xEE);
                    out.push(delay);
                    out.push(speed);
                    out.extend_from_slice(&depth.to_le_bytes());
                    out.push(cnt);
                }
            } else {
                diagnostics.push(Diagnostic::error(
                    span_of(chars, tok_start, i),
                    "Expected parameters after 'M'",
                ));
            }
            continue;
        }

        // SSG mixer P0/P1/P2 & noise period w0..31
        if c == 'P' {
            let tok_start = i;
            i += 1;
            ssg_mode = if let Some((val, next_i)) = parse_uint(chars, i) {
                i = next_i;
                val.clamp(0, 2) as u8
            } else {
                0
            };
            out.extend_from_slice(&[0xED, ssg_mode, ssg_nfreq]);
            emit_token_range(tokens, chars, tok_start, i, TokenKind::Mixer);
            continue;
        }
        if c == 'w' {
            let tok_start = i;
            i += 1;
            ssg_nfreq = if let Some((val, next_i)) = parse_uint(chars, i) {
                i = next_i;
                val.clamp(0, 31) as u8
            } else {
                12
            };
            out.extend_from_slice(&[0xED, ssg_mode, ssg_nfreq]);
            emit_token_range(tokens, chars, tok_start, i, TokenKind::Mixer);
            continue;
        }

        diagnostics.push(Diagnostic::error(
            span_of(chars, i, i + 1),
            format!("Unknown MML token '{c}' in ch {ch_idx}"),
        ));
        i += 1;
    }

    for frame in rep_stack {
        diagnostics.push(Diagnostic::error(
            span_of(chars, frame.start_src_idx, frame.start_src_idx + 1),
            format!("Unclosed '[' in channel {ch_idx}"),
        ));
    }
    if loop_abs_off != 0 && loop_ticks == 0 {
        diagnostics.push(Diagnostic::error(
            span_of(chars, loop_src_idx, loop_src_idx + 1),
            format!("Zero-tick loop after 'L' in channel {ch_idx}"),
        ));
    }

    out.push(0xF1);
    out.extend_from_slice(&loop_abs_off.to_le_bytes());
    (out, total_ticks)
}

/// Analyze and compile MML text, collecting all diagnostics, semantic tokens, and voice info.
pub fn analyze(text: &str) -> AnalysisResult {
    let mut diagnostics: Vec<Diagnostic> = Vec::new();
    let mut tokens: Vec<SemanticToken> = Vec::new();
    let mut voices: BTreeMap<usize, VoiceInfo> = BTreeMap::new();
    let mut init_tb = bpm_to_timer_b(120);

    // Build comment-stripped lines while recording comment semantic tokens.
    // We preserve exact 0-based (line, col) positions.
    let mut stripped_lines: Vec<Vec<(char, u32)>> = Vec::new();
    for (line_idx, raw_line) in text.split('\n').enumerate() {
        let line_no = line_idx as u32;
        let line_clean = raw_line.strip_suffix('\r').unwrap_or(raw_line);
        let mut chars_in_line: Vec<(char, u32)> = Vec::new();
        let mut col = 0u32;
        let mut comment_start: Option<u32> = None;
        for ch in line_clean.chars() {
            if ch == ';' {
                comment_start = Some(col);
                break;
            }
            chars_in_line.push((ch, col));
            col += ch.len_utf16() as u32;
        }
        if let Some(cstart) = comment_start {
            let total_utf16: u32 = line_clean.chars().map(|c| c.len_utf16() as u32).sum();
            tokens.push(SemanticToken {
                line: line_no,
                start_col: cstart,
                length: total_utf16.saturating_sub(cstart),
                kind: TokenKind::Comment,
            });
        }
        stripped_lines.push(chars_in_line);
    }

    // Flatten stripped source with exact (line, col) so multi-line `@N = { ... }` blocks are parsed cleanly.
    let mut flat: Vec<SrcChar> = Vec::new();
    for (line_idx, line_chars) in stripped_lines.iter().enumerate() {
        let line_no = line_idx as u32;
        for &(ch, col) in line_chars {
            flat.push(SrcChar {
                ch,
                line: line_no,
                col,
            });
        }
        flat.push(SrcChar {
            ch: '\n',
            line: line_no,
            col: line_chars
                .last()
                .map(|&(ch, c)| c + ch.len_utf16() as u32)
                .unwrap_or(0),
        });
    }

    // Pass 1: Extract `@N = { ... }` voice definitions, blanking them out in `masked`.
    let mut masked = flat.clone();
    let mut idx = 0usize;
    while idx < flat.len() {
        if flat[idx].ch == '@' {
            let at_idx = idx;
            let mut p = idx + 1;
            if let Some((vnum, after_num)) = parse_uint(&flat, p) {
                p = after_num;
                while p < flat.len() && matches!(flat[p].ch, ' ' | '\t' | '\n') {
                    p += 1;
                }
                if p < flat.len() && flat[p].ch == '=' {
                    p += 1;
                    while p < flat.len() && matches!(flat[p].ch, ' ' | '\t' | '\n') {
                        p += 1;
                    }
                    if p < flat.len() && flat[p].ch == '{' {
                        let open_brace = p;
                        emit_token_range(&mut tokens, &flat, at_idx, after_num, TokenKind::VoiceDef);
                        p += 1;
                        let mut close_brace = None;
                        while p < flat.len() {
                            if flat[p].ch == '}' {
                                close_brace = Some(p);
                                break;
                            }
                            p += 1;
                        }
                        if let Some(cb) = close_brace {
                            // Parse all integers inside { ... }
                            let mut nums: Vec<i32> = Vec::new();
                            let mut q = open_brace + 1;
                            while q < cb {
                                if let Some((val, next_q)) = parse_int(&flat, q) {
                                    emit_token_range(&mut tokens, &flat, q, next_q, TokenKind::Number);
                                    nums.push(val);
                                    q = next_q;
                                } else {
                                    q += 1;
                                }
                            }
                            if (vnum as usize) > MAX_VOICE_ID {
                                diagnostics.push(Diagnostic::error(
                                    span_of(&flat, at_idx, after_num),
                                    format!("FM voice ID @{vnum} exceeds maximum ({MAX_VOICE_ID})"),
                                ));
                            } else {
                                match pack_opn_voice(&nums) {
                                    Ok((bytes, fb, con, format)) => {
                                        voices.insert(
                                            vnum as usize,
                                            VoiceInfo {
                                                id: vnum as usize,
                                                format,
                                                fb,
                                                con,
                                                bytes,
                                                def_line: flat[at_idx].line,
                                            },
                                        );
                                    }
                                    Err(msg) => {
                                        diagnostics.push(Diagnostic::error(
                                            span_of(&flat, at_idx, cb + 1),
                                            msg,
                                        ));
                                    }
                                }
                            }
                            // Blank out the voice definition (keeping newlines)
                            for m in &mut masked[at_idx..=cb] {
                                if m.ch != '\n' {
                                    m.ch = ' ';
                                }
                            }
                            idx = cb + 1;
                            continue;
                        } else {
                            diagnostics.push(Diagnostic::error(
                                span_of(&flat, at_idx, open_brace + 1),
                                "Unclosed '{' in FM voice definition",
                            ));
                            break;
                        }
                    }
                }
            }
        }
        idx += 1;
    }

    // Pass 2: Split masked source back into lines and collect directives & channel MML.
    let mut ch_mml: [Vec<SrcChar>; 6] = Default::default();
    let mut line_start = 0usize;
    while line_start < masked.len() {
        let mut line_end = line_start;
        while line_end < masked.len() && masked[line_end].ch != '\n' {
            line_end += 1;
        }
        let slice = &masked[line_start..line_end];
        // Trim leading/trailing whitespace
        let mut s = 0usize;
        while s < slice.len() && matches!(slice[s].ch, ' ' | '\t') {
            s += 1;
        }
        let mut e = slice.len();
        while e > s && matches!(slice[e - 1].ch, ' ' | '\t') {
            e -= 1;
        }
        let trimmed = &slice[s..e];
        if !trimmed.is_empty() {
            if trimmed[0].ch == '#' {
                // Directive line
                let line_str: String = trimmed.iter().map(|sc| sc.ch).collect();
                emit_token_range(&mut tokens, trimmed, 0, trimmed.len(), TokenKind::Directive);
                let lower = line_str.to_ascii_lowercase();
                if let Some(rest) = lower.strip_prefix("#tempo") {
                    if rest.starts_with(|c: char| c.is_ascii_whitespace()) {
                        let trimmed_rest = rest.trim_start();
                        let digits: String = trimmed_rest
                            .chars()
                            .take_while(|c| c.is_ascii_digit())
                            .collect();
                        if !digits.is_empty() {
                            if let Ok(bpm) = digits.parse::<i32>() {
                                init_tb = bpm_to_timer_b(bpm);
                            }
                        }
                    }
                }
            } else {
                // Match ^([A-F]+)\s+(.*)$
                let mut ch_end = 0usize;
                while ch_end < trimmed.len() && matches!(trimmed[ch_end].ch, 'A'..='F') {
                    ch_end += 1;
                }
                if ch_end > 0 && ch_end < trimmed.len() && matches!(trimmed[ch_end].ch, ' ' | '\t')
                {
                    emit_token_range(&mut tokens, trimmed, 0, ch_end, TokenKind::Channel);
                    let mut body_start = ch_end;
                    while body_start < trimmed.len() && matches!(trimmed[body_start].ch, ' ' | '\t')
                    {
                        body_start += 1;
                    }
                    let body = &trimmed[body_start..];
                    for ch_sc in &trimmed[..ch_end] {
                        let ci = (ch_sc.ch as u8 - b'A') as usize;
                        ch_mml[ci].push(SrcChar {
                            ch: ' ',
                            line: ch_sc.line,
                            col: ch_sc.col,
                        });
                        ch_mml[ci].extend_from_slice(body);
                    }
                } else {
                    let line_str: String = trimmed.iter().map(|sc| sc.ch).collect();
                    diagnostics.push(Diagnostic::error(
                        span_of(trimmed, 0, trimmed.len()),
                        format!("Unrecognized MML line: {line_str:?}"),
                    ));
                }
            }
        }
        line_start = line_end + 1;
    }

    // Build voice table
    let max_v = voices.keys().next_back().map(|k| k + 1).unwrap_or(0);
    let voice_tbl_off: u16 = if max_v > 0 { 20 } else { 0 };
    let mut voice_blob: Vec<u8> = Vec::with_capacity(max_v * 26);
    for vi in 0..max_v {
        if let Some(info) = voices.get(&vi) {
            voice_blob.extend_from_slice(&info.bytes);
        } else {
            voice_blob.extend_from_slice(&DEFAULT_VOICE);
        }
    }

    // Compile each channel
    let mut cur_off = 20 + voice_blob.len();
    let mut ch_offs = [0u16; 6];
    let mut ch_blobs: Vec<Vec<u8>> = Vec::new();
    let mut channel_ticks = [0u32; 6];

    for ci in 0..6 {
        let has_content = ch_mml[ci].iter().any(|sc| !sc.ch.is_ascii_whitespace());
        if has_content {
            match u16::try_from(cur_off) {
                Ok(off16) => ch_offs[ci] = off16,
                Err(_) => {
                    diagnostics.push(Diagnostic::error(
                        span_of(&ch_mml[ci], 0, 1),
                        format!("Channel {ci} offset {cur_off} exceeds 16-bit .mdxt limit (65535 B)"),
                    ));
                }
            }
            let (cb, ticks) = compile_channel(
                &ch_mml[ci],
                ci,
                cur_off,
                max_v,
                &mut diagnostics,
                &mut tokens,
            );
            cur_off += cb.len();
            ch_blobs.push(cb);
            channel_ticks[ci] = ticks;
        }
    }

    // Sort & remove duplicate/overlapping semantic tokens (required by LSP spec)
    tokens.sort_by_key(|t| (t.line, t.start_col, t.length));
    let mut non_overlapping: Vec<SemanticToken> = Vec::with_capacity(tokens.len());
    for tok in tokens {
        if tok.length == 0 {
            continue;
        }
        if let Some(prev) = non_overlapping.last() {
            if prev.line == tok.line && tok.start_col < prev.start_col + prev.length {
                continue;
            }
        }
        non_overlapping.push(tok);
    }
    let tokens = non_overlapping;

    let binary = if diagnostics.is_empty() {
        let mut out = Vec::with_capacity(cur_off);
        out.extend_from_slice(b"MDXT");
        out.push(6);
        out.push(init_tb);
        out.extend_from_slice(&voice_tbl_off.to_le_bytes());
        for off in ch_offs {
            out.extend_from_slice(&off.to_le_bytes());
        }
        out.extend_from_slice(&voice_blob);
        for cb in ch_blobs {
            out.extend_from_slice(&cb);
        }
        Some(out)
    } else {
        None
    };

    AnalysisResult {
        binary,
        diagnostics,
        tokens,
        voices,
        channel_ticks,
        init_tb,
    }
}

/// Compile MML text into `.mdxt` binary, returning all diagnostics on error.
pub fn compile_mml(text: &str) -> Result<Vec<u8>, Vec<Diagnostic>> {
    let res = analyze(text);
    match res.binary {
        Some(bin) => Ok(bin),
        None => Err(res.diagnostics),
    }
}
