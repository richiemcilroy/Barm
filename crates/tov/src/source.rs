use std::path::PathBuf;

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub struct FileId(pub u32);

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Span {
    pub file: FileId,
    pub start: u32,
    pub end: u32,
}

impl Span {
    pub fn new(file: FileId, start: u32, end: u32) -> Span {
        Span { file, start, end }
    }

    pub fn to(self, other: Span) -> Span {
        Span { file: self.file, start: self.start.min(other.start), end: self.end.max(other.end) }
    }

    pub fn empty_at_end(self) -> Span {
        Span { file: self.file, start: self.end, end: self.end }
    }

    pub fn empty_at_start(self) -> Span {
        Span { file: self.file, start: self.start, end: self.start }
    }
}

pub struct SourceFile {
    pub path: PathBuf,
    /// Path as shown in diagnostics (relative to the working directory when possible).
    pub name: String,
    pub text: String,
    line_starts: Vec<u32>,
}

impl SourceFile {
    pub fn new(path: PathBuf, name: String, text: String) -> SourceFile {
        let mut line_starts = vec![0];
        for (i, b) in text.bytes().enumerate() {
            if b == b'\n' {
                line_starts.push(i as u32 + 1);
            }
        }
        SourceFile { path, name, text, line_starts }
    }

    /// 1-based line and column (column counted in characters).
    pub fn line_col(&self, pos: u32) -> (u32, u32) {
        let line = match self.line_starts.binary_search(&pos) {
            Ok(i) => i,
            Err(i) => i - 1,
        };
        let start = self.line_starts[line] as usize;
        let pos = (pos as usize).min(self.text.len());
        let col = self.text[start..pos].chars().count();
        (line as u32 + 1, col as u32 + 1)
    }

    pub fn line_text(&self, line: u32) -> &str {
        let i = (line - 1) as usize;
        let start = self.line_starts[i] as usize;
        let end = self.line_starts.get(i + 1).map(|&e| e as usize).unwrap_or(self.text.len());
        self.text[start..end].trim_end_matches(['\n', '\r'])
    }

    pub fn slice(&self, span: Span) -> &str {
        &self.text[span.start as usize..span.end as usize]
    }

    pub fn line_count(&self) -> usize {
        self.line_starts.len()
    }
}

#[derive(Default)]
pub struct SourceMap {
    pub files: Vec<SourceFile>,
}

impl SourceMap {
    pub fn add(&mut self, file: SourceFile) -> FileId {
        self.files.push(file);
        FileId(self.files.len() as u32 - 1)
    }

    pub fn get(&self, id: FileId) -> &SourceFile {
        &self.files[id.0 as usize]
    }

    pub fn slice(&self, span: Span) -> &str {
        self.get(span.file).slice(span)
    }
}
