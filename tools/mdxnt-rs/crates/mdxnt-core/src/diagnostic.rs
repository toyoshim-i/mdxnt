use std::fmt;

/// 0-based line and UTF-16 column span (LSP-compatible).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Span {
    pub start_line: u32,
    pub start_col: u32,
    pub end_line: u32,
    pub end_col: u32,
}

impl Span {
    pub fn single(line: u32, col: u32) -> Self {
        Self {
            start_line: line,
            start_col: col,
            end_line: line,
            end_col: col + 1,
        }
    }

    pub fn line_range(line: u32, start_col: u32, end_col: u32) -> Self {
        Self {
            start_line: line,
            start_col,
            end_line: line,
            end_col: end_col.max(start_col + 1),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    Error,
    Warning,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Diagnostic {
    pub span: Span,
    pub severity: Severity,
    pub message: String,
}

impl Diagnostic {
    pub fn error(span: Span, message: impl Into<String>) -> Self {
        Self {
            span,
            severity: Severity::Error,
            message: message.into(),
        }
    }

    pub fn warning(span: Span, message: impl Into<String>) -> Self {
        Self {
            span,
            severity: Severity::Warning,
            message: message.into(),
        }
    }
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sev = match self.severity {
            Severity::Error => "error",
            Severity::Warning => "warning",
        };
        write!(
            f,
            "{}:{}: {}: {}",
            self.span.start_line + 1,
            self.span.start_col + 1,
            sev,
            self.message
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TokenKind {
    Comment,
    Directive,
    Channel,
    VoiceDef,
    Note,
    Rest,
    Octave,
    Length,
    Tempo,
    VoiceSelect,
    Volume,
    Gate,
    Tie,
    Repeat,
    Loop,
    Modulation,
    Mixer,
    Number,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SemanticToken {
    pub line: u32,
    pub start_col: u32,
    pub length: u32,
    pub kind: TokenKind,
}
