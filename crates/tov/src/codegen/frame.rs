//! Async functions keep all their state in a frame, so they can return at an `await` and resume
//! there later (see `asyncfn.rs`). Their body is generated as ordinary C first; `lower` then
//! moves every local declaration into the frame, rewrites uses to `F->name`, and turns `return`
//! into "store the result; finished". Nothing else changes, so every construct compiles exactly
//! as it does in a synchronous function. Anything it can't parse is an error, never wrong code.

use std::collections::HashMap;

#[derive(Clone, Copy, PartialEq, Debug)]
enum Kind {
    Ident,
    Punct,
    /// Whitespace, comments, literals and numbers: copied as they are.
    Other,
}

fn tokenize(src: &str) -> Vec<(Kind, &str)> {
    let b = src.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let start = i;
        let c = b[i];
        let kind = if c.is_ascii_whitespace() {
            while i < b.len() && b[i].is_ascii_whitespace() {
                i += 1;
            }
            Kind::Other
        } else if c == b'/' && b.get(i + 1) == Some(&b'*') {
            i += 2;
            while i + 1 < b.len() && !(b[i] == b'*' && b[i + 1] == b'/') {
                i += 1;
            }
            i = (i + 2).min(b.len());
            Kind::Other
        } else if c == b'/' && b.get(i + 1) == Some(&b'/') {
            while i < b.len() && b[i] != b'\n' {
                i += 1;
            }
            Kind::Other
        } else if c == b'"' || c == b'\'' {
            i += 1;
            while i < b.len() && b[i] != c {
                if b[i] == b'\\' {
                    i += 1;
                }
                i += 1;
            }
            i = (i + 1).min(b.len());
            Kind::Other
        } else if c.is_ascii_alphabetic() || c == b'_' {
            while i < b.len() && (b[i].is_ascii_alphanumeric() || b[i] == b'_') {
                i += 1;
            }
            Kind::Ident
        } else if c.is_ascii_digit() || (c == b'.' && b.get(i + 1).is_some_and(|d| d.is_ascii_digit())) {
            let hex = c == b'0' && matches!(b.get(i + 1), Some(b'x' | b'X'));
            i += 1;
            while i < b.len() {
                let d = b[i];
                if d.is_ascii_alphanumeric() || d == b'_' || d == b'.' {
                    i += 1;
                } else if (d == b'+' || d == b'-') && !hex && matches!(b[i - 1], b'e' | b'E' | b'p' | b'P') {
                    i += 1;
                } else {
                    break;
                }
            }
            Kind::Other
        } else {
            const OPS: &[&str] = &["<<=", ">>=", "...", "->", "++", "--", "&&", "||", "<<", ">>", "<=", ">=", "==", "!=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="];
            let rest = &src[i..];
            let len = OPS.iter().find(|op| rest.starts_with(**op)).map(|op| op.len()).unwrap_or_else(|| rest.chars().next().map(|ch| ch.len_utf8()).unwrap_or(1));
            i += len;
            Kind::Punct
        };
        out.push((kind, &src[start..i]));
    }
    out
}

/// Words that start statements (never a declaration's type).
const STATEMENT_WORDS: &[&str] = &["return", "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "goto", "sizeof", "TV_SUSPEND"];

/// A local moved into the frame.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Field {
    /// The C type (`tv_str`, `tv_int *`, ...).
    pub ty: String,
    pub name: String,
}

pub(crate) struct Lowered {
    pub fields: Vec<Field>,
    pub body: String,
}

/// A declaration found at token index `start`: its type, declarators and where it ends (the `;`).
struct Decl {
    ty: String,
    /// (pointer stars, name, initializer token range)
    declarators: Vec<(usize, String, Option<(usize, usize)>)>,
    end: usize,
}

/// Index of the next significant token at or after `i`.
fn sig(toks: &[(Kind, &str)], mut i: usize) -> usize {
    while i < toks.len() && toks[i].0 == Kind::Other && (toks[i].1.trim().is_empty() || toks[i].1.starts_with("/*") || toks[i].1.starts_with("//")) {
        i += 1;
    }
    i
}

fn parse_decl(toks: &[(Kind, &str)], start: usize) -> Result<Option<Decl>, String> {
    let mut i = sig(toks, start);
    let mut ty = String::new();
    if i < toks.len() && toks[i].1 == "const" {
        ty.push_str("const ");
        i = sig(toks, i + 1);
    }
    if i < toks.len() && toks[i].1 == "struct" {
        ty.push_str("struct ");
        i = sig(toks, i + 1);
    }
    let Some(&(Kind::Ident, tname)) = toks.get(i) else { return Ok(None) };
    if STATEMENT_WORDS.contains(&tname) {
        return Ok(None);
    }
    ty.push_str(tname);
    // `T name`, `T *name`: the type is followed by stars and a name.
    let mut j = sig(toks, i + 1);
    while j < toks.len() && toks[j].1 == "*" {
        j = sig(toks, j + 1);
    }
    let Some(&(Kind::Ident, _)) = toks.get(j) else { return Ok(None) };
    let after = sig(toks, j + 1);
    if !matches!(toks.get(after).map(|t| t.1), Some("=" | ";" | "," | "[")) {
        return Ok(None);
    }
    let mut declarators = Vec::new();
    let mut k = i + 1;
    loop {
        k = sig(toks, k);
        let mut stars = 0;
        while k < toks.len() && toks[k].1 == "*" {
            stars += 1;
            k = sig(toks, k + 1);
        }
        let Some(&(Kind::Ident, name)) = toks.get(k) else { return Err(format!("unexpected declarator near `{}`", toks.get(k).map(|t| t.1).unwrap_or("<end>"))) };
        k = sig(toks, k + 1);
        match toks.get(k).map(|t| t.1) {
            Some("[") => return Err(format!("array local `{name}` in an async function")),
            Some("=") => {
                let from = k + 1;
                let mut depth = 0i32;
                let mut e = from;
                while e < toks.len() {
                    match toks[e].1 {
                        "(" | "[" | "{" => depth += 1,
                        ")" | "]" | "}" => depth -= 1,
                        "," | ";" if depth == 0 => break,
                        _ => {}
                    }
                    e += 1;
                }
                declarators.push((stars, name.to_string(), Some((from, e))));
                k = e;
            }
            _ => declarators.push((stars, name.to_string(), None)),
        }
        match toks.get(k).map(|t| t.1) {
            Some(",") => k += 1,
            Some(";") => return Ok(Some(Decl { ty, declarators, end: k })),
            other => return Err(format!("unexpected `{}` in a declaration", other.unwrap_or("<end>"))),
        }
    }
}

/// Moves `body`'s locals into the frame. `params` are already frame fields (named as in the body).
pub(crate) fn lower(body: &str, params: &[String]) -> Result<Lowered, String> {
    let toks = tokenize(body);
    // Pass 1: find the declarations (at statement starts and in `for (` headers).
    let mut decls: HashMap<usize, Decl> = HashMap::new();
    let mut fields: Vec<Field> = Vec::new();
    let mut depth = 0i32;
    let mut at_start = true;
    let mut i = 0;
    while i < toks.len() {
        let (kind, text) = toks[i];
        if kind == Kind::Other {
            i += 1;
            continue;
        }
        let for_init = text == "(" && i > 0 && toks[..i].iter().rev().find(|t| t.0 != Kind::Other).map(|t| t.1) == Some("for");
        if at_start && depth == 0 || for_init {
            let from = if for_init { i + 1 } else { i };
            if let Some(d) = parse_decl(&toks, from)? {
                for (stars, name, _) in &d.declarators {
                    let ty = format!("{}{}", d.ty, " *".repeat(*stars).replace("* *", "**"));
                    let ty = ty.trim_end().to_string();
                    match fields.iter().find(|f| f.name == *name) {
                        Some(f) if f.ty != ty => return Err(format!("`{name}` is declared twice with different types (`{}`, `{ty}`)", f.ty)),
                        Some(_) => {}
                        None => fields.push(Field { ty, name: name.clone() }),
                    }
                }
                let end = d.end;
                decls.insert(from, d);
                if for_init {
                    depth += 1;
                    i = end;
                    continue;
                }
                i = end + 1;
                at_start = true;
                continue;
            }
        }
        match text {
            "(" | "[" => depth += 1,
            ")" | "]" => depth -= 1,
            _ => {}
        }
        at_start = depth == 0 && matches!(text, ";" | "{" | "}");
        i += 1;
    }
    let names: std::collections::HashSet<&str> = fields.iter().map(|f| f.name.as_str()).chain(params.iter().map(|p| p.as_str())).collect();

    // Pass 2: rewrite.
    let mut out = String::with_capacity(body.len() + body.len() / 4);
    let mut prev_sig: &str = "";
    let mut i = 0;
    let emit_range = |out: &mut String, from: usize, to: usize, names: &std::collections::HashSet<&str>| {
        let mut prev: &str = "";
        for &(kind, text) in &toks[from..to] {
            if kind == Kind::Ident && names.contains(text) && prev != "." && prev != "->" {
                out.push_str("F->");
            }
            out.push_str(text);
            if kind != Kind::Other {
                prev = text;
            }
        }
    };
    while i < toks.len() {
        if let Some(d) = decls.get(&i) {
            let mut first = true;
            for (stars, name, init) in &d.declarators {
                let Some((from, to)) = *init else { continue };
                if !first {
                    out.push(' ');
                }
                first = false;
                out.push_str("F->");
                out.push_str(name);
                out.push_str(" = ");
                let init_start = sig(&toks, from);
                if toks.get(init_start).map(|t| t.1) == Some("{") {
                    // A brace initializer becomes a compound literal.
                    out.push('(');
                    out.push_str(&d.ty);
                    out.push_str(&"*".repeat(*stars));
                    out.push(')');
                }
                emit_range(&mut out, init_start, to, &names);
                out.push(';');
            }
            if first {
                out.push(';');
            }
            i = d.end + 1;
            prev_sig = ";";
            continue;
        }
        let (kind, text) = toks[i];
        if kind == Kind::Ident && text == "return" {
            // `return;` / `return e;` → store the result, then report "finished".
            let mut e = i + 1;
            let mut depth = 0i32;
            while e < toks.len() {
                match toks[e].1 {
                    "(" | "[" | "{" => depth += 1,
                    ")" | "]" | "}" => depth -= 1,
                    ";" if depth == 0 => break,
                    _ => {}
                }
                e += 1;
            }
            let has_value = toks[i + 1..e].iter().any(|t| t.0 != Kind::Other);
            if has_value {
                out.push_str("{ F->ret =");
                emit_range(&mut out, i + 1, e, &names);
                out.push_str("; return true; }");
            } else {
                out.push_str("return true;");
            }
            i = e + 1;
            prev_sig = ";";
            continue;
        }
        if kind == Kind::Ident && names.contains(text) && prev_sig != "." && prev_sig != "->" {
            out.push_str("F->");
        }
        out.push_str(text);
        if kind != Kind::Other {
            prev_sig = text;
        }
        i += 1;
    }
    Ok(Lowered { fields, body: out })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lowered(body: &str, params: &[&str]) -> (String, Vec<(String, String)>) {
        let params: Vec<String> = params.iter().map(|s| s.to_string()).collect();
        let l = lower(body, &params).unwrap();
        (l.body, l.fields.into_iter().map(|f| (f.ty, f.name)).collect())
    }

    #[test]
    fn moves_declarations_and_rewrites_uses() {
        let (b, f) = lowered("    tv_int t1 = p0 + 1;\n    tv_str t2 = tv_str_concat(t1, x.t1);\n    t1++;\n", &["p0"]);
        assert_eq!(b, "    F->t1 = F->p0 + 1;\n    F->t2 = tv_str_concat(F->t1, x.t1);\n    F->t1++;\n");
        assert_eq!(f, vec![("tv_int".into(), "t1".into()), ("tv_str".into(), "t2".into())]);
    }

    #[test]
    fn pointers_multiple_declarators_and_no_initializer() {
        let (b, f) = lowered("tv_int c3 = 0; void *c3k, *c3v;\ntv_str r;\nr = s;\n", &[]);
        assert_eq!(b, "F->c3 = 0; ;\n;\nF->r = s;\n");
        assert_eq!(f, vec![("tv_int".into(), "c3".into()), ("void *".into(), "c3k".into()), ("void *".into(), "c3v".into()), ("tv_str".into(), "r".into())]);
    }

    #[test]
    fn for_headers_braces_and_returns() {
        let (b, f) = lowered("for (tv_int i4 = 0; i4 < n; i4++) { E7 ev = { {-1, NULL}, &(i4) }; f(&ev); }\nreturn i4;\nreturn;\n", &[]);
        assert_eq!(b, "for (F->i4 = 0; F->i4 < n; F->i4++) { F->ev = (E7){ {-1, NULL}, &(F->i4) }; f(&F->ev); }\n{ F->ret = F->i4; return true; }\nreturn true;\n");
        assert_eq!(f, vec![("tv_int".into(), "i4".into()), ("E7".into(), "ev".into())]);
    }

    #[test]
    fn leaves_strings_members_labels_and_expression_statements() {
        let (b, _) = lowered("tv_int x = 1;\nputs(\"x = 1;\"); s->x = x; F->pc = &&L2; L2:; *p = x; (void)x;\n", &[]);
        assert_eq!(b, "F->x = 1;\nputs(\"x = 1;\"); s->x = F->x; F->pc = &&L2; L2:; *p = F->x; (void)F->x;\n");
    }

    #[test]
    fn rejects_what_it_cannot_move() {
        assert!(lower("char buf[16];\n", &[]).is_err());
        assert!(lower("tv_int a = 1;\n{ double a = 2; }\n", &[]).is_err());
    }
}
