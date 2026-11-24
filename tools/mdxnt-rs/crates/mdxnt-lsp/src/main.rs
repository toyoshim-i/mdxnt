use std::collections::HashMap;

use mdxnt_core::{analyze, AnalysisResult, Severity, TokenKind};
use tokio::sync::RwLock;
use tower_lsp::jsonrpc::Result;
use tower_lsp::lsp_types::*;
use tower_lsp::{Client, LanguageServer, LspService, Server};

const SEMANTIC_TOKEN_LEGEND: &[SemanticTokenType] = &[
    SemanticTokenType::COMMENT,   // 0: Comment
    SemanticTokenType::MACRO,     // 1: Directive (#tempo)
    SemanticTokenType::NAMESPACE, // 2: Channel (A..F)
    SemanticTokenType::CLASS,     // 3: VoiceDef (@0 = {...})
    SemanticTokenType::VARIABLE,  // 4: Note (c..b)
    SemanticTokenType::PARAMETER, // 5: Rest (r)
    SemanticTokenType::OPERATOR,  // 6: Octave (o, <, >)
    SemanticTokenType::NUMBER,    // 7: Length / Number
    SemanticTokenType::KEYWORD,   // 8: Tempo / Loop / Repeat
    SemanticTokenType::FUNCTION,  // 9: VoiceSelect (@N)
    SemanticTokenType::PROPERTY,  // 10: Volume / Gate / Tie
    SemanticTokenType::DECORATOR, // 11: Modulation / Mixer (D, _, M, P, w)
];

fn token_type_index(kind: TokenKind) -> u32 {
    match kind {
        TokenKind::Comment => 0,
        TokenKind::Directive => 1,
        TokenKind::Channel => 2,
        TokenKind::VoiceDef => 3,
        TokenKind::Note => 4,
        TokenKind::Rest => 5,
        TokenKind::Octave => 6,
        TokenKind::Length | TokenKind::Number => 7,
        TokenKind::Tempo | TokenKind::Repeat | TokenKind::Loop => 8,
        TokenKind::VoiceSelect => 9,
        TokenKind::Volume | TokenKind::Gate | TokenKind::Tie => 10,
        TokenKind::Modulation | TokenKind::Mixer => 11,
    }
}

struct DocState {
    text: String,
    analysis: AnalysisResult,
}

struct Backend {
    client: Client,
    docs: RwLock<HashMap<Url, DocState>>,
}

impl Backend {
    fn new(client: Client) -> Self {
        Self {
            client,
            docs: RwLock::new(HashMap::new()),
        }
    }

    async fn update_document(&self, uri: Url, text: String, version: Option<i32>) {
        let analysis = analyze(&text);
        let lsp_diags: Vec<Diagnostic> = analysis
            .diagnostics
            .iter()
            .map(|d| Diagnostic {
                range: Range {
                    start: Position {
                        line: d.span.start_line,
                        character: d.span.start_col,
                    },
                    end: Position {
                        line: d.span.end_line,
                        character: d.span.end_col,
                    },
                },
                severity: Some(match d.severity {
                    Severity::Error => DiagnosticSeverity::ERROR,
                    Severity::Warning => DiagnosticSeverity::WARNING,
                }),
                source: Some("mdxnt".to_string()),
                message: d.message.clone(),
                ..Default::default()
            })
            .collect();

        {
            let mut map = self.docs.write().await;
            map.insert(uri.clone(), DocState { text, analysis });
        }

        self.client
            .publish_diagnostics(uri, lsp_diags, version)
            .await;
    }
}

/// Convert a UTF-16 column offset on a line into a `char` index, returning `None` if past EOL.
fn utf16_col_to_char_idx(chars: &[char], utf16_col: u32) -> Option<usize> {
    let mut acc = 0u32;
    for (idx, &ch) in chars.iter().enumerate() {
        let w = ch.len_utf16() as u32;
        if utf16_col < acc + w {
            return Some(idx);
        }
        acc += w;
    }
    None
}

/// Returns `(prefix_start, prefix_end_excl, has_fm, has_ssg)` if the line starts with `[A-F]+\s`.
fn parse_channel_prefix(chars: &[char]) -> Option<(usize, usize, bool, bool)> {
    let mut s = 0;
    while s < chars.len() && matches!(chars[s], ' ' | '\t') {
        s += 1;
    }
    let mut e = s;
    let mut has_fm = false;
    let mut has_ssg = false;
    while e < chars.len() && matches!(chars[e], 'A'..='F') {
        if matches!(chars[e], 'A'..='C') {
            has_fm = true;
        } else {
            has_ssg = true;
        }
        e += 1;
    }
    if e > s && e < chars.len() && matches!(chars[e], ' ' | '\t') {
        Some((s, e, has_fm, has_ssg))
    } else {
        None
    }
}

fn hover_doc_for_position(doc: &DocState, pos: Position) -> Option<String> {
    let line_str = doc.text.split('\n').nth(pos.line as usize)?;
    let line_clean = line_str.strip_suffix('\r').unwrap_or(line_str);
    let chars: Vec<char> = line_clean.chars().collect();
    let col = utf16_col_to_char_idx(&chars, pos.character)?;

    // Ignore comments (anything at or after ';')
    if let Some(semi_idx) = chars.iter().position(|&c| c == ';') {
        if col >= semi_idx {
            return None;
        }
    }
    let code_chars = match chars.iter().position(|&c| c == ';') {
        Some(semi_idx) => &chars[..semi_idx],
        None => &chars[..],
    };

    let ch_prefix = parse_channel_prefix(code_chars);
    let is_ssg_only = matches!(ch_prefix, Some((_, _, false, true)));

    // Check if hovering on `@t<N>`, `@q<N>`, or `@<N>`
    let mut start = col;
    while start > 0 && code_chars[start].is_ascii_digit() {
        start -= 1;
    }
    if matches!(code_chars[start], 't' | 'q') && start > 0 && code_chars[start - 1] == '@' {
        start -= 1;
    }
    if code_chars[start] == '@' {
        let rest: String = code_chars[start + 1..].iter().collect();
        if rest.starts_with('t') {
            return Some("**`@t<N>`** — Set raw YM2203 Timer-B register value (`0..255`).".into());
        }
        if rest.starts_with('q') {
            return Some(
                "**`@q<N>`** — Fixed key-off tail gate time (`0..127` ticks before note end)."
                    .into(),
            );
        }
        let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
        if let Ok(vnum) = digits.parse::<usize>() {
            if is_ssg_only {
                if vnum < 6 {
                    let ssg_desc = match vnum {
                        0 => "Flat organ / SE sustain",
                        1 => "Bright synth lead / strings",
                        2 => "Punchy bass / pluck",
                        3 => "Expressive melody / bell-lead",
                        4 => "Soft pad / chord",
                        5 => "Short percussive hi-hat / snare noise",
                        _ => "",
                    };
                    return Some(format!(
                        "**SSG Envelope `@{vnum}`** (built-in for channels D–F)\n\n{ssg_desc}"
                    ));
                }
            } else if let Some(v) = doc.analysis.voices.get(&vnum) {
                return Some(format!(
                    "**FM Voice `@{vnum}`** (line {})\n\n- Format: {}\n- Feedback (FB): `{}`\n- Algorithm (CON): `{}`",
                    v.def_line + 1,
                    v.format,
                    v.fb,
                    v.con
                ));
            } else if vnum < 6 {
                let ssg_desc = match vnum {
                    0 => "Flat organ / SE sustain",
                    1 => "Bright synth lead / strings",
                    2 => "Punchy bass / pluck",
                    3 => "Expressive melody / bell-lead",
                    4 => "Soft pad / chord",
                    5 => "Short percussive hi-hat / snare noise",
                    _ => "",
                };
                return Some(format!(
                    "**SSG Envelope `@{vnum}`** (built-in for channels D–F)\n\n{ssg_desc}"
                ));
            }
        }
    }

    let in_channel_prefix = matches!(ch_prefix, Some((s, e, _, _)) if col >= s && col < e);
    let c = code_chars[col];
    let doc_text = match c {
        '#' => "**`#tempo <BPM>`** — Set initial song tempo (`30..300` BPM).",
        'A' | 'B' | 'C' if in_channel_prefix => {
            "**FM Channel (`A`–`C`)** — YM2203 4-operator FM channels 1–3."
        }
        'D' => {
            if in_channel_prefix {
                "**SSG Channel (`D`–`F`)** — YM2203 SSG channels A–C."
            } else {
                "**`D[+-]<N>`** — Detune offset (signed 16-bit F-num / SSG TP delta)."
            }
        }
        'E' | 'F' if in_channel_prefix => {
            "**SSG Channel (`D`–`F`)** — YM2203 SSG channels A–C."
        }
        't' => "**`t<BPM>`** — Set tempo in BPM (`30..300`), converted to YM2203 Timer-B.",
        'o' => "**`o<0..7>`** — Set current octave.",
        '<' => "**`<`** — Octave down (`-1`).",
        '>' => "**`>`** — Octave up (`+1`).",
        'l' => "**`l<len>`** — Set default note/rest length (e.g. `l4`, `l8.`, `l%12`).",
        'v' => "**`v<0..15>`** — Set channel volume (logarithmic TL on FM, 0..15 on SSG).",
        '(' => "**`(`** — Decrease volume by 1.",
        ')' => "**`)`** — Increase volume by 1.",
        'q' => "**`q<1..8>`** — Gate time ratio (`q8` = full length, `q4` = 50%).",
        '&' => "**`&`** — Tie / slur into the next note (suppresses re-keyon).",
        '^' => "**`^`** — Extend previous note/rest duration (e.g. `c4^8`).",
        'L' => "**`L`** — Infinite loop point for this channel.",
        '[' | '/' | ']' => {
            "**`[ ... / ... ]N`** — Repeat block `N` times (up to 4 nested levels; `/` escapes on final iteration)."
        }
        '_' => "**`_[+-]<N>`** — Pitch sweep / portamento delta per tick.",
        'M' => "**`M delay,speed,depth,count`** — Software pitch LFO (`M0` to disable).",
        'P' => "**`P<0..2>`** — SSG mixer mode (`0`=Tone, `1`=Noise, `2`=Tone+Noise).",
        'w' => "**`w<0..31>`** — SSG noise frequency period (`0..31`).",
        _ => return None,
    };
    Some(doc_text.to_string())
}

#[tower_lsp::async_trait]
impl LanguageServer for Backend {
    async fn initialize(&self, _: InitializeParams) -> Result<InitializeResult> {
        Ok(InitializeResult {
            capabilities: ServerCapabilities {
                text_document_sync: Some(TextDocumentSyncCapability::Kind(
                    TextDocumentSyncKind::FULL,
                )),
                hover_provider: Some(HoverProviderCapability::Simple(true)),
                completion_provider: Some(CompletionOptions {
                    trigger_characters: Some(vec!["@".into(), "#".into()]),
                    ..Default::default()
                }),
                semantic_tokens_provider: Some(
                    SemanticTokensServerCapabilities::SemanticTokensOptions(
                        SemanticTokensOptions {
                            legend: SemanticTokensLegend {
                                token_types: SEMANTIC_TOKEN_LEGEND.to_vec(),
                                token_modifiers: vec![],
                            },
                            full: Some(SemanticTokensFullOptions::Bool(true)),
                            range: None,
                            ..Default::default()
                        },
                    ),
                ),
                ..Default::default()
            },
            server_info: Some(ServerInfo {
                name: "mdxnt-lsp".to_string(),
                version: Some(env!("CARGO_PKG_VERSION").to_string()),
            }),
        })
    }

    async fn initialized(&self, _: InitializedParams) {
        self.client
            .log_message(MessageType::INFO, "mdxnt-lsp initialized")
            .await;
    }

    async fn shutdown(&self) -> Result<()> {
        Ok(())
    }

    async fn did_open(&self, params: DidOpenTextDocumentParams) {
        self.update_document(
            params.text_document.uri,
            params.text_document.text,
            Some(params.text_document.version),
        )
        .await;
    }

    async fn did_change(&self, params: DidChangeTextDocumentParams) {
        if let Some(change) = params.content_changes.into_iter().last() {
            self.update_document(
                params.text_document.uri,
                change.text,
                Some(params.text_document.version),
            )
            .await;
        }
    }

    async fn did_close(&self, params: DidCloseTextDocumentParams) {
        {
            let mut map = self.docs.write().await;
            map.remove(&params.text_document.uri);
        }
        self.client
            .publish_diagnostics(params.text_document.uri, vec![], None)
            .await;
    }

    async fn semantic_tokens_full(
        &self,
        params: SemanticTokensParams,
    ) -> Result<Option<SemanticTokensResult>> {
        let map = self.docs.read().await;
        let Some(doc) = map.get(&params.text_document.uri) else {
            return Ok(None);
        };
        let mut data = Vec::with_capacity(doc.analysis.tokens.len());
        let mut prev_line = 0u32;
        let mut prev_start = 0u32;

        for tok in &doc.analysis.tokens {
            let delta_line = tok.line.saturating_sub(prev_line);
            let delta_start = if delta_line == 0 {
                tok.start_col.saturating_sub(prev_start)
            } else {
                tok.start_col
            };
            data.push(SemanticToken {
                delta_line,
                delta_start,
                length: tok.length,
                token_type: token_type_index(tok.kind),
                token_modifiers_bitset: 0,
            });
            prev_line = tok.line;
            prev_start = tok.start_col;
        }

        Ok(Some(SemanticTokensResult::Tokens(SemanticTokens {
            result_id: None,
            data,
        })))
    }

    async fn hover(&self, params: HoverParams) -> Result<Option<Hover>> {
        let map = self.docs.read().await;
        let Some(doc) = map.get(&params.text_document_position_params.text_document.uri) else {
            return Ok(None);
        };
        let Some(md) = hover_doc_for_position(doc, params.text_document_position_params.position)
        else {
            return Ok(None);
        };
        Ok(Some(Hover {
            contents: HoverContents::Markup(MarkupContent {
                kind: MarkupKind::Markdown,
                value: md,
            }),
            range: None,
        }))
    }

    async fn completion(&self, params: CompletionParams) -> Result<Option<CompletionResponse>> {
        let mut items: Vec<CompletionItem> = vec![
            CompletionItem {
                label: "#tempo".into(),
                kind: Some(CompletionItemKind::KEYWORD),
                detail: Some("Set initial BPM (30..300)".into()),
                insert_text: Some("#tempo 120".into()),
                ..Default::default()
            },
            CompletionItem {
                label: "@t".into(),
                kind: Some(CompletionItemKind::KEYWORD),
                detail: Some("Set raw OPN Timer-B value (0..255)".into()),
                ..Default::default()
            },
            CompletionItem {
                label: "@q".into(),
                kind: Some(CompletionItemKind::KEYWORD),
                detail: Some("Fixed tail gate time (0..127 ticks)".into()),
                ..Default::default()
            },
            CompletionItem {
                label: "M".into(),
                kind: Some(CompletionItemKind::FUNCTION),
                detail: Some("Software Pitch LFO: M delay,speed,depth,count".into()),
                insert_text: Some("M4,2,8,4".into()),
                ..Default::default()
            },
            CompletionItem {
                label: "P0".into(),
                kind: Some(CompletionItemKind::CONSTANT),
                detail: Some("SSG Mixer: Tone only".into()),
                ..Default::default()
            },
            CompletionItem {
                label: "P1".into(),
                kind: Some(CompletionItemKind::CONSTANT),
                detail: Some("SSG Mixer: Noise only".into()),
                ..Default::default()
            },
            CompletionItem {
                label: "P2".into(),
                kind: Some(CompletionItemKind::CONSTANT),
                detail: Some("SSG Mixer: Tone + Noise".into()),
                ..Default::default()
            },
        ];

        let map = self.docs.read().await;
        if let Some(doc) = map.get(&params.text_document_position.text_document.uri) {
            for v in doc.analysis.voices.values() {
                items.push(CompletionItem {
                    label: format!("@{}", v.id),
                    kind: Some(CompletionItemKind::CLASS),
                    detail: Some(format!("FM Voice (FB={}, CON={}, {})", v.fb, v.con, v.format)),
                    ..Default::default()
                });
            }
        }

        Ok(Some(CompletionResponse::Array(items)))
    }
}

#[tokio::main]
async fn main() {
    let stdin = tokio::io::stdin();
    let stdout = tokio::io::stdout();
    let (service, socket) = LspService::new(Backend::new);
    Server::new(stdin, stdout, socket).serve(service).await;
}
