//! A JavaScript tokenizer: enough to find module syntax (`import`, `export`, `require(...)`)
//! without being fooled by strings, template literals, comments or regular expressions.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// Identifiers and keywords (the source text says which).
    Ident,
    /// `#name` in classes.
    Private,
    /// String literal (the span includes the quotes).
    Str,
    Num,
    Regex,
    /// A whole template literal without substitutions, or the piece before the first `${`.
    TemplateHead,
    /// Between `}` and the next `${` of a template.
    TemplateMiddle,
    /// From `}` to the closing backtick.
    TemplateTail,
    /// A whole template literal without substitutions.
    Template,
    Punct,
}

#[derive(Clone, Copy, Debug)]
pub struct Tok {
    pub kind: Kind,
    pub start: u32,
    pub end: u32,
    /// A line break between this token and the previous one (for ASI).
    pub nl_before: bool,
}

impl Tok {
    pub fn text<'a>(&self, src: &'a str) -> &'a str {
        &src[self.start as usize..self.end as usize]
    }
}

#[derive(Debug)]
pub struct LexError {
    pub pos: u32,
    pub message: &'static str,
}

/// Keywords after which `/` starts a regular expression (an expression is expected).
const REGEX_AFTER: &[&str] = &["return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "throw", "case", "do", "else", "yield", "await", "extends"];

pub fn tokenize(src: &str) -> Result<Vec<Tok>, LexError> {
    let b = src.as_bytes();
    let n = b.len();
    let mut toks: Vec<Tok> = Vec::with_capacity(n / 4);
    let mut i = 0usize;
    // `#!` line at the very start.
    if b.starts_with(b"#!") {
        while i < n && b[i] != b'\n' {
            i += 1;
        }
    }
    // For each open `{`: whether it opened a template substitution `${`.
    let mut braces: Vec<bool> = Vec::new();
    let mut nl = false;
    let err = |pos: usize, message: &'static str| LexError { pos: pos as u32, message };
    while i < n {
        let c = b[i];
        // whitespace and line terminators
        if c == b'\n' || c == b'\r' {
            nl = true;
            i += 1;
            continue;
        }
        if c == b' ' || c == b'\t' || c == 0x0b || c == 0x0c {
            i += 1;
            continue;
        }
        if c >= 0x80 {
            // U+2028/U+2029 are line terminators; other non-ASCII whitespace (NBSP, BOM, ...) is skipped.
            let ch = src[i..].chars().next().unwrap();
            if ch == '\u{2028}' || ch == '\u{2029}' {
                nl = true;
                i += ch.len_utf8();
                continue;
            }
            if ch.is_whitespace() || ch == '\u{feff}' {
                i += ch.len_utf8();
                continue;
            }
        }
        // comments
        if c == b'/' && i + 1 < n && b[i + 1] == b'/' {
            while i < n && b[i] != b'\n' && b[i] != b'\r' {
                i += 1;
            }
            continue;
        }
        if c == b'/' && i + 1 < n && b[i + 1] == b'*' {
            let start = i;
            i += 2;
            loop {
                if i + 1 >= n {
                    return Err(err(start, "unterminated comment"));
                }
                if b[i] == b'\n' || b[i] == b'\r' {
                    nl = true;
                }
                if b[i] == b'*' && b[i + 1] == b'/' {
                    i += 2;
                    break;
                }
                i += 1;
            }
            continue;
        }
        let start = i;
        let nl_before = std::mem::take(&mut nl);
        let push = |kind: Kind, end: usize, toks: &mut Vec<Tok>| toks.push(Tok { kind, start: start as u32, end: end as u32, nl_before });
        // identifiers / keywords (including `\u` escapes and non-ASCII letters)
        if c == b'_' || c == b'$' || c.is_ascii_alphabetic() || c == b'\\' || c >= 0x80 || c == b'#' {
            let kind = if c == b'#' {
                i += 1;
                Kind::Private
            } else {
                Kind::Ident
            };
            while i < n {
                let d = b[i];
                if d == b'_' || d == b'$' || d.is_ascii_alphanumeric() {
                    i += 1;
                } else if d == b'\\' {
                    i += 2;
                    if i < n && b[i] == b'{' {
                        while i < n && b[i] != b'}' {
                            i += 1;
                        }
                        i += 1;
                    } else {
                        i += 4;
                    }
                } else if d >= 0x80 {
                    let ch = src[i..].chars().next().unwrap();
                    if ch.is_alphanumeric() || ch == '\u{200c}' || ch == '\u{200d}' {
                        i += ch.len_utf8();
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
            if i == start || (kind == Kind::Private && i == start + 1) {
                return Err(err(start, "unexpected character"));
            }
            push(kind, i.min(n), &mut toks);
            continue;
        }
        // numbers (decimal, hex/octal/binary, bigint, separators, exponents, leading dot)
        if c.is_ascii_digit() || (c == b'.' && i + 1 < n && b[i + 1].is_ascii_digit()) {
            if c == b'0' && i + 1 < n && matches!(b[i + 1], b'x' | b'X' | b'o' | b'O' | b'b' | b'B') {
                i += 2;
                while i < n && (b[i].is_ascii_hexdigit() || b[i] == b'_') {
                    i += 1;
                }
            } else {
                while i < n && (b[i].is_ascii_digit() || b[i] == b'_' || b[i] == b'.') {
                    i += 1;
                }
                if i < n && (b[i] == b'e' || b[i] == b'E') {
                    i += 1;
                    if i < n && (b[i] == b'+' || b[i] == b'-') {
                        i += 1;
                    }
                    while i < n && (b[i].is_ascii_digit() || b[i] == b'_') {
                        i += 1;
                    }
                }
            }
            if i < n && b[i] == b'n' {
                i += 1;
            }
            push(Kind::Num, i, &mut toks);
            continue;
        }
        // strings
        if c == b'"' || c == b'\'' {
            i += 1;
            loop {
                if i >= n {
                    return Err(err(start, "unterminated string"));
                }
                let d = b[i];
                if d == b'\\' {
                    i += 2;
                    continue;
                }
                if d == c {
                    i += 1;
                    break;
                }
                if d == b'\n' {
                    return Err(err(start, "unterminated string"));
                }
                i += 1;
            }
            push(Kind::Str, i, &mut toks);
            continue;
        }
        // template literals: the head up to `${` or the closing backtick
        if c == b'`' {
            i += 1;
            let (end, subst) = scan_template(b, i).ok_or_else(|| err(start, "unterminated template literal"))?;
            i = end;
            if subst {
                braces.push(true);
                push(Kind::TemplateHead, i, &mut toks);
            } else {
                push(Kind::Template, i, &mut toks);
            }
            continue;
        }
        // `}` closing a template substitution continues the template
        if c == b'}' && braces.last() == Some(&true) {
            braces.pop();
            i += 1;
            let (end, subst) = scan_template(b, i).ok_or_else(|| err(start, "unterminated template literal"))?;
            i = end;
            if subst {
                braces.push(true);
                push(Kind::TemplateMiddle, i, &mut toks);
            } else {
                push(Kind::TemplateTail, i, &mut toks);
            }
            continue;
        }
        // regular expressions
        if c == b'/' && regex_allowed(src, &toks) {
            i += 1;
            let mut class = false;
            loop {
                if i >= n || b[i] == b'\n' {
                    return Err(err(start, "unterminated regular expression"));
                }
                let d = b[i];
                if d == b'\\' {
                    i += 2;
                    continue;
                }
                if d == b'[' {
                    class = true;
                } else if d == b']' {
                    class = false;
                } else if d == b'/' && !class {
                    i += 1;
                    break;
                }
                i += 1;
            }
            while i < n && (b[i].is_ascii_alphabetic()) {
                i += 1;
            }
            push(Kind::Regex, i, &mut toks);
            continue;
        }
        // punctuators (longest match)
        let len = punct_len(&b[i..]);
        if len == 0 {
            return Err(err(start, "unexpected character"));
        }
        if c == b'{' {
            braces.push(false);
        } else if c == b'}' {
            braces.pop();
        }
        i += len;
        push(Kind::Punct, i, &mut toks);
    }
    Ok(toks)
}

/// Scans template characters from `i` (after a backtick or `}`): returns (end, whether it
/// stopped at `${`).
fn scan_template(b: &[u8], mut i: usize) -> Option<(usize, bool)> {
    while i < b.len() {
        match b[i] {
            b'\\' => i += 2,
            b'`' => return Some((i + 1, false)),
            b'$' if i + 1 < b.len() && b[i + 1] == b'{' => return Some((i + 2, true)),
            _ => i += 1,
        }
    }
    None
}

/// Is a `/` here the start of a regular expression (rather than division)?
fn regex_allowed(src: &str, toks: &[Tok]) -> bool {
    let Some(prev) = toks.last() else { return true };
    match prev.kind {
        Kind::Num | Kind::Str | Kind::Regex | Kind::Template | Kind::TemplateTail | Kind::Private => false,
        Kind::TemplateHead | Kind::TemplateMiddle => true,
        Kind::Ident => REGEX_AFTER.contains(&prev.text(src)),
        Kind::Punct => !matches!(prev.text(src), ")" | "]" | "}" | "++" | "--"),
    }
}

fn punct_len(s: &[u8]) -> usize {
    const P4: &[&[u8]] = &[b">>>="];
    const P3: &[&[u8]] = &[b"===", b"!==", b"**=", b"<<=", b">>=", b">>>", b"...", b"&&=", b"||=", b"??="];
    const P2: &[&[u8]] = &[b"=>", b"==", b"!=", b"<=", b">=", b"&&", b"||", b"??", b"?.", b"++", b"--", b"+=", b"-=", b"*=", b"/=", b"%=", b"&=", b"|=", b"^=", b"<<", b">>", b"**"];
    for p in P4 {
        if s.starts_with(p) {
            return 4;
        }
    }
    for p in P3 {
        if s.starts_with(p) {
            return 3;
        }
    }
    for p in P2 {
        if s.starts_with(p) {
            // `?.` followed by a digit is `?` then a number (`a?.5:b`)
            if *p == b"?." && s.len() > 2 && s[2].is_ascii_digit() {
                return 1;
            }
            return 2;
        }
    }
    if b"{}()[];,<>+-*/%&|^!~?:=.@".contains(&s[0]) {
        return 1;
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kinds(src: &str) -> Vec<(Kind, &str)> {
        tokenize(src).unwrap().iter().map(|t| (t.kind, t.text(src))).collect()
    }

    #[test]
    fn regex_vs_division() {
        let k = kinds("a = b / c / d; x = /re[/]x/g.test(y); return /a/");
        assert!(k.contains(&(Kind::Regex, "/re[/]x/g")));
        assert!(k.contains(&(Kind::Regex, "/a/")));
        assert_eq!(k.iter().filter(|t| t.0 == Kind::Regex).count(), 2);
    }

    #[test]
    fn templates_nest() {
        let k = kinds("`a${ `b${c}` + {d: 1}.d }e` + f");
        assert_eq!(k[0], (Kind::TemplateHead, "`a${"));
        assert!(k.contains(&(Kind::TemplateTail, "}e`")));
        assert_eq!(k.last().unwrap(), &(Kind::Ident, "f"));
    }

    #[test]
    fn comments_and_strings() {
        let k = kinds("// import x from 'y'\nconst s = \"require('z')\" /* export */ ; require('w')");
        assert_eq!(k.iter().filter(|t| t.1 == "require").count(), 1);
        assert!(k.contains(&(Kind::Str, "'w'")));
    }
}
