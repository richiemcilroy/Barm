use crate::source::{SourceMap, Span};
use std::fmt::Write;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Applicability {
    /// Correct by construction; tools may apply it without review.
    Safe,
    /// Probably what was meant; review before applying.
    Maybe,
    /// Contains a placeholder the author must fill in.
    Placeholder,
}

impl Applicability {
    pub fn as_str(self) -> &'static str {
        match self {
            Applicability::Safe => "safe",
            Applicability::Maybe => "maybe",
            Applicability::Placeholder => "placeholder",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Fix {
    pub label: String,
    pub edits: Vec<(Span, String)>,
    pub applicability: Applicability,
}

#[derive(Clone, Debug)]
pub struct Note {
    pub label: &'static str,
    pub text: String,
}

#[derive(Clone, Debug)]
pub struct Diagnostic {
    pub code: &'static str,
    pub span: Span,
    pub message: String,
    pub notes: Vec<Note>,
    pub fixes: Vec<Fix>,
}

impl Diagnostic {
    pub fn new(code: &'static str, span: Span, message: impl Into<String>) -> Diagnostic {
        Diagnostic { code, span, message: message.into(), notes: Vec::new(), fixes: Vec::new() }
    }

    pub fn note(mut self, label: &'static str, text: impl Into<String>) -> Diagnostic {
        self.notes.push(Note { label, text: text.into() });
        self
    }

    pub fn fix(mut self, applicability: Applicability, label: impl Into<String>, span: Span, text: impl Into<String>) -> Diagnostic {
        self.fixes.push(Fix { label: label.into(), edits: vec![(span, text.into())], applicability });
        self
    }

    pub fn fix_edits(mut self, applicability: Applicability, label: impl Into<String>, edits: Vec<(Span, String)>) -> Diagnostic {
        self.fixes.push(Fix { label: label.into(), edits, applicability });
        self
    }

    pub fn render(&self, sm: &SourceMap, out: &mut String) {
        let file = sm.get(self.span.file);
        let (line, col) = file.line_col(self.span.start);
        let _ = writeln!(out, "error[{}] {}:{}:{}: {}", self.code, file.name, line, col, self.message);
        let text = file.line_text(line);
        if !text.trim().is_empty() {
            let _ = writeln!(out, "  {line:>3} | {}", text.trim_end());
        }
        for note in &self.notes {
            let _ = writeln!(out, "  {}: {}", note.label, note.text);
        }
        for fix in &self.fixes {
            let _ = writeln!(out, "  fix[{}]: {}", fix.applicability.as_str(), fix.label);
        }
    }
}

pub fn render_text(diags: &[Diagnostic], sm: &SourceMap) -> String {
    let mut out = String::new();
    for d in diags {
        d.render(sm, &mut out);
        out.push('\n');
    }
    out
}

fn json_str(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn json_pos(out: &mut String, sm: &SourceMap, span: Span, pos: u32) {
    let (line, col) = sm.get(span.file).line_col(pos);
    let _ = write!(out, "{{\"line\":{line},\"col\":{col}}}");
}

pub fn render_json(diags: &[Diagnostic], sm: &SourceMap, files_checked: usize) -> String {
    let mut out = String::from("{\"diagnostics\":[");
    for (i, d) in diags.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str("{\"code\":");
        json_str(&mut out, d.code);
        out.push_str(",\"file\":");
        json_str(&mut out, &sm.get(d.span.file).name);
        out.push_str(",\"start\":");
        json_pos(&mut out, sm, d.span, d.span.start);
        out.push_str(",\"end\":");
        json_pos(&mut out, sm, d.span, d.span.end);
        out.push_str(",\"message\":");
        json_str(&mut out, &d.message);
        out.push_str(",\"notes\":[");
        for (j, n) in d.notes.iter().enumerate() {
            if j > 0 {
                out.push(',');
            }
            out.push_str("{\"label\":");
            json_str(&mut out, n.label);
            out.push_str(",\"text\":");
            json_str(&mut out, &n.text);
            out.push('}');
        }
        out.push_str("],\"fixes\":[");
        for (j, f) in d.fixes.iter().enumerate() {
            if j > 0 {
                out.push(',');
            }
            out.push_str("{\"label\":");
            json_str(&mut out, &f.label);
            out.push_str(",\"applicability\":");
            json_str(&mut out, f.applicability.as_str());
            out.push_str(",\"edits\":[");
            for (k, (span, text)) in f.edits.iter().enumerate() {
                if k > 0 {
                    out.push(',');
                }
                out.push_str("{\"start\":");
                json_pos(&mut out, sm, *span, span.start);
                out.push_str(",\"end\":");
                json_pos(&mut out, sm, *span, span.end);
                out.push_str(",\"text\":");
                json_str(&mut out, text);
                out.push('}');
            }
            out.push_str("]}");
        }
        out.push_str("]}");
    }
    let _ = write!(out, "],\"errors\":{},\"files\":{}}}", diags.len(), files_checked);
    out
}

/// Levenshtein distance, used for "did you mean" suggestions.
pub fn edit_distance(a: &str, b: &str) -> usize {
    let b: Vec<char> = b.chars().collect();
    let mut prev: Vec<usize> = (0..=b.len()).collect();
    let mut cur = vec![0; b.len() + 1];
    for (i, ca) in a.chars().enumerate() {
        cur[0] = i + 1;
        for (j, &cb) in b.iter().enumerate() {
            let sub = prev[j] + usize::from(!ca.eq_ignore_ascii_case(&cb));
            cur[j + 1] = sub.min(prev[j + 1] + 1).min(cur[j] + 1);
        }
        std::mem::swap(&mut prev, &mut cur);
    }
    prev[b.len()]
}

/// Candidates close enough to `name` to suggest, best first.
pub fn similar<'a>(name: &str, candidates: impl Iterator<Item = &'a str>) -> Vec<&'a str> {
    let max = match name.chars().count() {
        0..=2 => 1,
        3..=5 => 2,
        _ => 3,
    };
    let mut scored: Vec<(usize, &str)> = candidates
        .filter(|c| *c != name)
        .map(|c| (edit_distance(name, c), c))
        .filter(|(d, c)| *d <= max || c.eq_ignore_ascii_case(name))
        .collect();
    scored.sort();
    scored.dedup_by(|a, b| a.1 == b.1);
    scored.into_iter().take(3).map(|(_, c)| c).collect()
}
