pub mod compiler;
pub mod diagnostic;
pub mod header;
pub mod voice;

pub use compiler::{analyze, bpm_to_timer_b, compile_mml, AnalysisResult, MAX_REP_DEPTH, TPQ, WHOLE_TICKS};
pub use diagnostic::{Diagnostic, SemanticToken, Severity, Span, TokenKind};
pub use header::{default_symbol_name, emit_c_header};
pub use voice::{pack_opn_voice, VoiceInfo, DEFAULT_VOICE};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_bpm_to_timer_b() {
        assert_eq!(bpm_to_timer_b(120), 220);
        assert_eq!(bpm_to_timer_b(150), 227);
        assert_eq!(bpm_to_timer_b(60), 184);
    }

    #[test]
    fn test_valid_compilation() {
        let mml = r#"
#tempo 120
@0 = {
  5, 4,
  31,10, 0, 7, 2,20,0, 1,0,
  31, 8, 2, 7, 2, 0,0, 1,0,
  31,10, 0, 7, 2,24,0, 2,0,
  31, 8, 2, 7, 2, 0,0, 1,0
}
A @0 o4 l8 v12 [c d [e f]2]2 L g4 a4
D @3 o5 l16 v11 c%0 d4 M4,2,8,3 & v15 e4
"#;
        let bin = compile_mml(mml).expect("should compile");
        assert_eq!(&bin[..4], b"MDXT");
        assert_eq!(bin[4], 6);
        assert_eq!(bin[5], 220);
    }

    #[test]
    fn test_invalid_cases() {
        let v0 = "@0 = {0,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0}\n";
        let cases = [
            ("missing @t", "A @t c4".to_string()),
            ("L inside repeat", "A [L c4]2".to_string()),
            ("0-tick repeat", "A [v12]2 c4".to_string()),
            ("0-tick L loop", "A c4 L".to_string()),
            ("repeat depth > 4", "A [[[[[c4]2]2]2]2]2".to_string()),
            ("FM voice OOB with voices", format!("{v0}A @1 c4")),
            ("FM voice OOB with 0 voices", "A @0 c4".to_string()),
            ("SSG envelope OOB", "D @6 c4".to_string()),
            ("FM voice ID > 255", "@256 = {0,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0, 31,0,0,0,0,0,0,1,0}\nA c4".to_string()),
            ("Detune i16 overflow", "A D40000 c4".to_string()),
            ("Sweep i16 overflow", "A _-40000 c4".to_string()),
            ("Note ticks overflow", "A c%2147483647^%2147483647".to_string()),
        ];
        for (name, src) in cases {
            assert!(
                compile_mml(&src).is_err(),
                "Expected error for case: {name}"
            );
        }
    }

    #[test]
    fn test_tempo_parity_and_non_overlapping_tokens() {
        let a = analyze("#tempo -120\nA c4\n");
        assert_eq!(a.init_tb, bpm_to_timer_b(120));

        let b = analyze("#tempo 150bpm\nA c4\n");
        assert_eq!(b.init_tb, bpm_to_timer_b(150));

        let c = analyze("#title @0 = { 1, 2 } suffix\nA c4\n");
        for w in c.tokens.windows(2) {
            if w[0].line == w[1].line {
                assert!(w[0].start_col + w[0].length <= w[1].start_col);
            }
        }
    }
}
