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
    /// JSX (with `LexOpts::jsx`): `<` opening a tag.
    JsxOpen,
    /// `</` opening a closing tag.
    JsxClose,
    /// A tag or attribute name (`div`, `Foo.Bar`, `aria-label`, `xlink:href`).
    JsxName,
    /// An attribute's string value (quotes included; no escapes).
    JsxStr,
    /// Text between tags.
    JsxText,
    /// `>` ending a tag.
    JsxEnd,
    /// `/>` ending a self-closing tag.
    JsxSelfClose,
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
    tokenize_with(src, &LexOpts::default())
}

#[derive(Default)]
pub struct LexOpts<'f> {
    /// JSX (`.jsx`, `.tsx`): `<` where an expression starts opens an element.
    pub jsx: bool,
    /// How a `/` at these byte offsets reads (true: a regular expression), sorted: the parser's
    /// corrections of the regex/division guess.
    pub slashes: &'f [(u32, bool)],
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Brace {
    Block,
    /// `${` in a template
    Template,
    /// `{` in a JSX tag (an attribute value or spread)
    JsxTag,
    /// `{` among a JSX element's children
    JsxChild,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Js,
    JsxTag,
    JsxChildren,
}

pub fn tokenize_with(src: &str, opts: &LexOpts) -> Result<Vec<Tok>, LexError> {
    let b = src.as_bytes();
    let n = b.len();
    let mut from = 0;
    if b.starts_with(b"#!") {
        while from < n && b[from] != b'\n' {
            from += 1;
        }
    }
    let mut toks: Vec<Tok> = Vec::with_capacity((n - from) / 4);
    let mut i = from;
    let mut braces: Vec<Brace> = Vec::new();
    let mut nl = false;
    let err = |pos: usize, message: &'static str| LexError { pos: pos as u32, message };
    // JSX: the mode, the element depth of each JSX expression being read (they nest through
    // `{...}`), and whether the current tag is a closing one
    let mut mode = Mode::Js;
    let mut depths: Vec<u32> = Vec::new();
    let mut closing = false;
    while i < n {
        let c = b[i];
        if mode == Mode::JsxChildren {
            let start = i;
            while i < n && b[i] != b'<' && b[i] != b'{' {
                i += 1;
            }
            if i > start {
                toks.push(Tok { kind: Kind::JsxText, start: start as u32, end: i as u32, nl_before: false });
            }
            if i >= n {
                return Err(err(start, "unterminated JSX element"));
            }
            let start = i;
            if b[i] == b'{' {
                braces.push(Brace::JsxChild);
                mode = Mode::Js;
                i += 1;
                toks.push(Tok { kind: Kind::Punct, start: start as u32, end: i as u32, nl_before: false });
            } else {
                // `<` or `</`
                let mut j = i + 1;
                while j < n && matches!(b[j], b' ' | b'\t' | b'\n' | b'\r') {
                    j += 1;
                }
                closing = j < n && b[j] == b'/';
                i = if closing { j + 1 } else { i + 1 };
                toks.push(Tok { kind: if closing { Kind::JsxClose } else { Kind::JsxOpen }, start: start as u32, end: i as u32, nl_before: false });
                mode = Mode::JsxTag;
            }
            continue;
        }
        if mode == Mode::JsxTag && !matches!(c, b' ' | b'\t' | b'\n' | b'\r') {
            let start = i;
            let push = |kind: Kind, end: usize, toks: &mut Vec<Tok>| toks.push(Tok { kind, start: start as u32, end: end as u32, nl_before: false });
            match c {
                b'>' => {
                    i += 1;
                    push(Kind::JsxEnd, i, &mut toks);
                    let d = depths.last_mut().expect("in JSX");
                    if closing {
                        *d -= 1;
                    } else {
                        *d += 1;
                    }
                    if *d == 0 {
                        depths.pop();
                        mode = Mode::Js;
                    } else {
                        mode = Mode::JsxChildren;
                    }
                }
                b'/' if i + 1 < n && b[i + 1] == b'>' => {
                    i += 2;
                    push(Kind::JsxSelfClose, i, &mut toks);
                    if *depths.last().expect("in JSX") == 0 {
                        depths.pop();
                        mode = Mode::Js;
                    } else {
                        mode = Mode::JsxChildren;
                    }
                }
                b'/' if i + 1 < n && (b[i + 1] == b'*' || b[i + 1] == b'/') => {
                    // a comment inside a tag
                    if b[i + 1] == b'/' {
                        while i < n && b[i] != b'\n' {
                            i += 1;
                        }
                    } else {
                        i += 2;
                        while i + 1 < n && !(b[i] == b'*' && b[i + 1] == b'/') {
                            i += 1;
                        }
                        i += 2;
                    }
                }
                b'{' => {
                    i += 1;
                    braces.push(Brace::JsxTag);
                    mode = Mode::Js;
                    push(Kind::Punct, i, &mut toks);
                }
                b'=' => {
                    i += 1;
                    push(Kind::Punct, i, &mut toks);
                }
                b'"' | b'\'' => {
                    i += 1;
                    while i < n && b[i] != c {
                        i += 1;
                    }
                    if i >= n {
                        return Err(err(start, "unterminated JSX attribute string"));
                    }
                    i += 1;
                    push(Kind::JsxStr, i, &mut toks);
                }
                _ => {
                    while i < n && (b[i].is_ascii_alphanumeric() || matches!(b[i], b'_' | b'$' | b'-' | b':' | b'.') || b[i] >= 0x80) {
                        i += 1;
                    }
                    if i == start {
                        return Err(err(start, "unexpected character in a JSX tag"));
                    }
                    push(Kind::JsxName, i, &mut toks);
                }
            }
            continue;
        }
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
                braces.push(Brace::Template);
                push(Kind::TemplateHead, i, &mut toks);
            } else {
                push(Kind::Template, i, &mut toks);
            }
            continue;
        }
        // `}` closing a JSX expression returns to the tag or the children
        if c == b'}' && matches!(braces.last(), Some(Brace::JsxTag | Brace::JsxChild)) {
            mode = if braces.pop() == Some(Brace::JsxTag) { Mode::JsxTag } else { Mode::JsxChildren };
            i += 1;
            push(Kind::Punct, i, &mut toks);
            continue;
        }
        // JSX: `<` where an expression starts (not TypeScript's `<T,>(...) =>` or `<T extends U>`)
        if c == b'<' && opts.jsx && regex_allowed(src, &toks) && jsx_starts(b, i + 1) {
            i += 1;
            push(Kind::JsxOpen, i, &mut toks);
            depths.push(0);
            closing = false;
            mode = Mode::JsxTag;
            continue;
        }
        // `}` closing a template substitution continues the template
        if c == b'}' && braces.last() == Some(&Brace::Template) {
            braces.pop();
            i += 1;
            let (end, subst) = scan_template(b, i).ok_or_else(|| err(start, "unterminated template literal"))?;
            i = end;
            if subst {
                braces.push(Brace::Template);
                push(Kind::TemplateMiddle, i, &mut toks);
            } else {
                push(Kind::TemplateTail, i, &mut toks);
            }
            continue;
        }
        // regular expressions (or the parser's correction of the guess)
        let forced = if c == b'/' && !opts.slashes.is_empty() { opts.slashes.binary_search_by_key(&(i as u32), |s| s.0).ok().map(|k| opts.slashes[k].1) } else { None };
        if c == b'/' && forced.unwrap_or_else(|| regex_allowed(src, &toks)) {
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
            braces.push(Brace::Block);
        } else if c == b'}' {
            braces.pop();
        }
        i += len;
        push(Kind::Punct, i, &mut toks);
    }
    if mode != Mode::Js {
        return Err(err(n, "unterminated JSX element"));
    }
    Ok(toks)
}

/// After a `<` where an expression starts: a JSX element (`<div`, `<Foo.Bar`, `<>`) rather than
/// TypeScript type parameters (`<T,>`, `<T extends U>`).
fn jsx_starts(b: &[u8], mut i: usize) -> bool {
    let n = b.len();
    if i < n && b[i] == b'>' {
        return true;
    }
    if i >= n || !(b[i].is_ascii_alphabetic() || b[i] == b'_' || b[i] == b'$') {
        return false;
    }
    while i < n && (b[i].is_ascii_alphanumeric() || matches!(b[i], b'_' | b'$' | b'-' | b':' | b'.')) {
        i += 1;
    }
    while i < n && matches!(b[i], b' ' | b'\t' | b'\n' | b'\r') {
        i += 1;
    }
    // `<T,` / `<T extends` / `<T = X>`: type parameters
    !(i < n && (b[i] == b',' || (b[i] == b'=' && i + 1 < n && b[i + 1] != b'{' && b[i + 1] != b'"' && b[i + 1] != b'\'') || b[i..].starts_with(b"extends ")))
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
        // (a keyword after `.` is a property name: `l.else / v`)
        Kind::Ident => REGEX_AFTER.contains(&prev.text(src)) && !(toks.len() >= 2 && matches!(toks[toks.len() - 2].text(src), "." | "?.") && toks[toks.len() - 2].kind == Kind::Punct),
        Kind::Punct => !matches!(prev.text(src), ")" | "]" | "}" | "++" | "--"),
        // (an element that just ended is an operand)
        Kind::JsxEnd | Kind::JsxSelfClose => false,
        Kind::JsxOpen | Kind::JsxClose | Kind::JsxName | Kind::JsxStr | Kind::JsxText => true,
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

/// The string literals of `require("...")` calls (not `x.require(...)`), unescaped.
pub fn static_requires(src: &str) -> Result<Vec<String>, String> {
    let toks = tokenize(src).map_err(|e| format!("{} at byte {}", e.message, e.pos))?;
    let mut out = Vec::new();
    for (i, t) in toks.iter().enumerate() {
        if t.kind != Kind::Ident || t.text(src) != "require" {
            continue;
        }
        if i > 0 && is_member_dot(&toks[i - 1], src) {
            continue;
        }
        if let [open, arg, close, ..] = &toks[i + 1..]
            && open.text(src) == "(" && close.text(src) == ")" && (arg.kind == Kind::Str || arg.kind == Kind::Template)
        {
            out.push(unquote(arg.text(src)));
        }
    }
    Ok(out)
}

fn is_member_dot(t: &Tok, src: &str) -> bool {
    t.kind == Kind::Punct && (t.text(src) == "." || t.text(src) == "?.")
}

/// The value of a string literal token (quotes and common escapes).
pub fn unquote(lit: &str) -> String {
    let inner = &lit[1..lit.len() - 1];
    if !inner.contains('\\') {
        return inner.to_string();
    }
    let mut out = String::new();
    let mut chars = inner.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('n') => out.push('\n'),
            Some('t') => out.push('\t'),
            Some('r') => out.push('\r'),
            Some('0') => out.push('\0'),
            Some('u') => {
                let hex: String = chars.by_ref().take(4).collect();
                if let Some(ch) = u32::from_str_radix(&hex, 16).ok().and_then(char::from_u32) {
                    out.push(ch);
                }
            }
            Some('x') => {
                let hex: String = chars.by_ref().take(2).collect();
                if let Some(ch) = u32::from_str_radix(&hex, 16).ok().and_then(char::from_u32) {
                    out.push(ch);
                }
            }
            Some('\n') => {}
            Some(other) => out.push(other),
            None => {}
        }
    }
    out
}

/// `src` without comments and with whitespace collapsed: a line break stays where there was
/// one (automatic semicolons), a space only where tokens would otherwise merge.
pub fn minify(src: &str) -> Result<String, String> {
    let toks = tokenize(src).map_err(|e| format!("{} at byte {}", e.message, e.pos))?;
    let mut out = String::with_capacity(src.len() / 2);
    let mut prev: Option<&Tok> = None;
    let word = |c: u8| c.is_ascii_alphanumeric() || c == b'_' || c == b'$' || c == b'\\' || c >= 0x80;
    for t in &toks {
        let text = t.text(src);
        if let Some(p) = prev {
            let (a, b) = (*p.text(src).as_bytes().last().unwrap(), text.as_bytes()[0]);
            if t.nl_before {
                out.push('\n');
            } else if (word(a) && word(b))
                || (p.kind == Kind::Regex && word(b))
                || (p.kind == Kind::Num && b == b'.')
                || (a == b'+' && b == b'+')
                || (a == b'-' && (b == b'-' || b == b'>'))
                || (a == b'/' && (b == b'/' || b == b'*'))
                || (a == b'<' && b == b'!')
            {
                out.push(' ');
            }
        }
        out.push_str(text);
        prev = Some(t);
    }
    Ok(out)
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

    /// Minifying keeps every token and line break: re-tokenized, the output is the same.
    #[test]
    fn minify_keeps_tokens() {
        let src = "'use strict'\na = b - -c + +d / /re/g.exec(e) in f; x = 1 .toString(); y = a-- > b; if (a < !--b) {}\nlet z = `t${ 1 }u`\n/* c */ // d\nq";
        let min = minify(src).unwrap();
        let (a, b) = (tokenize(src).unwrap(), tokenize(&min).unwrap());
        assert_eq!(a.len(), b.len());
        for (x, y) in a.iter().zip(&b) {
            assert_eq!((x.text(src), x.kind, x.nl_before), (y.text(&min), y.kind, y.nl_before));
        }
    }
}
