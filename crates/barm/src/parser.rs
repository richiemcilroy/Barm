use crate::ast::*;
use crate::diag::{Applicability, Diagnostic};
use crate::intern::{Interner, Sym};
use crate::lexer::{self, Tok, Token};
use crate::source::{FileId, Span};

pub fn parse(src: &str, file: FileId, interner: &mut Interner) -> (Ast, Vec<Diagnostic>) {
    let mut diags = Vec::new();
    let lexed = lexer::lex(src, file, &mut diags);
    let mut p = Parser { src, toks: lexed.tokens, strings: lexed.strings, pos: 0, file, interner, ast: Ast::default(), diags };
    p.module();
    (p.ast, p.diags)
}

struct Parser<'a> {
    src: &'a str,
    toks: Vec<Token>,
    strings: Vec<String>,
    pos: usize,
    file: FileId,
    interner: &'a mut Interner,
    ast: Ast,
    diags: Vec<Diagnostic>,
}

impl<'a> Parser<'a> {
    // ---------------------------------------------------------------- tokens

    fn tok(&self) -> Token {
        self.toks[self.pos]
    }

    fn kind(&self) -> Tok {
        self.toks[self.pos].kind
    }

    fn nth(&self, n: usize) -> Tok {
        self.toks.get(self.pos + n).map(|t| t.kind).unwrap_or(Tok::Eof)
    }

    fn at(&self, k: Tok) -> bool {
        self.kind() == k
    }

    fn bump(&mut self) -> Token {
        let t = self.toks[self.pos];
        if t.kind != Tok::Eof {
            self.pos += 1;
        }
        t
    }

    fn eat(&mut self, k: Tok) -> bool {
        if self.at(k) {
            self.bump();
            true
        } else {
            false
        }
    }

    fn span_of(&self, t: Token) -> Span {
        Span::new(self.file, t.start, t.end)
    }

    fn cur_span(&self) -> Span {
        self.span_of(self.tok())
    }

    fn prev_span(&self) -> Span {
        if self.pos == 0 {
            return Span::new(self.file, 0, 0);
        }
        self.span_of(self.toks[self.pos - 1])
    }

    fn text(&self, t: Token) -> &'a str {
        &self.src[t.start as usize..t.end as usize]
    }

    fn at_word(&self, w: &str) -> bool {
        self.at(Tok::Ident) && self.text(self.tok()) == w
    }

    /// True when the current token is glued to the previous one (no whitespace between).
    fn adjacent(&self) -> bool {
        self.pos > 0 && self.toks[self.pos - 1].end == self.tok().start
    }

    fn found(&self) -> String {
        let t = self.tok();
        match t.kind {
            Tok::Eof => "end of file".to_string(),
            _ => format!("`{}`", self.text(t)),
        }
    }

    // ---------------------------------------------------------------- diagnostics

    fn err(&mut self, code: &'static str, span: Span, msg: impl Into<String>) {
        self.diags.push(Diagnostic::new(code, span, msg));
    }

    fn push(&mut self, d: Diagnostic) {
        self.diags.push(d);
    }

    fn expect(&mut self, k: Tok, context: &str) -> bool {
        if self.eat(k) {
            return true;
        }
        let msg = format!("expected {} {}, found {}", k.describe(), context, self.found());
        let span = self.cur_span();
        self.err("P0001", span, msg);
        false
    }

    /// Statement terminator: `;`, a line break, `}` or end of file.
    fn terminator(&mut self) {
        if self.eat(Tok::Semi) || self.at(Tok::RBrace) || self.at(Tok::Eof) || self.tok().nl_before {
            return;
        }
        let msg = format!("expected `;` or a line break after the statement, found {}", self.found());
        let span = self.cur_span();
        self.err("P0002", span, msg);
        self.recover_line();
    }

    /// Skips to the start of the next statement.
    fn recover_line(&mut self) {
        let mut depth = 0i32;
        loop {
            match self.kind() {
                Tok::Eof => return,
                Tok::Semi if depth == 0 => {
                    self.bump();
                    return;
                }
                Tok::RBrace if depth == 0 => return,
                Tok::LBrace | Tok::LParen | Tok::LBracket => depth += 1,
                Tok::RBrace | Tok::RParen | Tok::RBracket => depth -= 1,
                _ => {}
            }
            self.bump();
            if depth <= 0 && self.tok().nl_before {
                return;
            }
        }
    }

    /// Skips an unsupported declaration: up to and including its `{ ... }` body, or to the end of the line.
    fn skip_decl(&mut self) {
        loop {
            match self.kind() {
                Tok::Eof => return,
                Tok::LBrace => {
                    self.skip_balanced();
                    return;
                }
                Tok::Semi => {
                    self.bump();
                    return;
                }
                _ => {
                    self.bump();
                    if self.tok().nl_before && !matches!(self.kind(), Tok::LBrace) {
                        return;
                    }
                }
            }
        }
    }

    fn skip_balanced(&mut self) {
        let mut depth = 0i32;
        loop {
            match self.kind() {
                Tok::Eof => return,
                Tok::LBrace | Tok::LParen | Tok::LBracket => depth += 1,
                Tok::RBrace | Tok::RParen | Tok::RBracket => {
                    depth -= 1;
                    if depth <= 0 {
                        self.bump();
                        return;
                    }
                }
                _ => {}
            }
            self.bump();
        }
    }

    /// Runs `f`; if it fails or reports errors, rewinds as if it never ran.
    fn speculate<T>(&mut self, f: impl FnOnce(&mut Self) -> Option<T>) -> Option<T> {
        let (pos, nd, ne, ns, nt) = (self.pos, self.diags.len(), self.ast.exprs.len(), self.ast.stmts.len(), self.ast.types.len());
        let r = f(self);
        if r.is_some() && self.diags.len() == nd {
            return r;
        }
        self.pos = pos;
        self.diags.truncate(nd);
        self.ast.exprs.truncate(ne);
        self.ast.stmts.truncate(ns);
        self.ast.types.truncate(nt);
        None
    }

    /// Index of the token closing the paren at `self.pos`.
    fn matching_paren(&self) -> Option<usize> {
        let mut depth = 0i32;
        for i in self.pos..self.toks.len() {
            match self.toks[i].kind {
                Tok::LParen => depth += 1,
                Tok::RParen => {
                    depth -= 1;
                    if depth == 0 {
                        return Some(i);
                    }
                }
                Tok::Eof => return None,
                _ => {}
            }
        }
        None
    }

    // ---------------------------------------------------------------- nodes

    fn mk_expr(&mut self, kind: ExprKind, span: Span) -> ExprId {
        self.ast.exprs.push(Expr { kind, span });
        self.ast.exprs.len() as ExprId - 1
    }

    fn mk_stmt(&mut self, kind: StmtKind, span: Span) -> StmtId {
        self.ast.stmts.push(Stmt { kind, span });
        self.ast.stmts.len() as StmtId - 1
    }

    fn mk_ty(&mut self, kind: TypeExprKind, span: Span) -> TypeId {
        self.ast.types.push(TypeExpr { kind, span });
        self.ast.types.len() as TypeId - 1
    }

    fn expr_span(&self, e: ExprId) -> Span {
        self.ast.exprs[e as usize].span
    }

    fn ident(&mut self, context: &str) -> Option<(Sym, Span)> {
        if self.at(Tok::Ident) {
            let t = self.bump();
            let sym = self.interner.intern(self.text(t));
            return Some((sym, self.span_of(t)));
        }
        let msg = if self.kind().is_keyword() {
            format!("expected {context}, found keyword {}", self.found())
        } else {
            format!("expected {context}, found {}", self.found())
        };
        let span = self.cur_span();
        self.err("P0001", span, msg);
        None
    }

    /// Property names may be any identifier or keyword (`s.type`, `{ default: 1 }`).
    fn prop_name(&mut self) -> Option<(Sym, Span)> {
        if self.at(Tok::Ident) || self.kind().is_keyword() {
            let t = self.bump();
            let sym = self.interner.intern(self.text(t));
            return Some((sym, self.span_of(t)));
        }
        None
    }

    // ---------------------------------------------------------------- items

    fn module(&mut self) {
        while !self.at(Tok::Eof) {
            let start = self.pos;
            self.item();
            if self.pos == start {
                self.bump();
            }
        }
    }

    fn item(&mut self) {
        let start = self.cur_span();
        let mut exported = false;
        if self.at(Tok::Export) {
            let export_span = self.cur_span();
            self.bump();
            exported = true;
            if self.at(Tok::Default) {
                let span = export_span.to(self.cur_span());
                self.push(
                    Diagnostic::new("X0011", span, "default exports are not supported")
                        .note("instead", "export the declaration by name and import it with `import { name }`")
                        .fix(Applicability::Maybe, "remove `default`", Span::new(self.file, export_span.end, self.cur_span().end), ""),
                );
                self.bump();
            } else if self.at(Tok::LBrace) || self.at(Tok::Star) {
                self.push(
                    Diagnostic::new("X0013", export_span.to(self.cur_span()), "export lists and re-exports are not supported")
                        .note("instead", "put `export` on each declaration, and import from the module that declares it"),
                );
                self.skip_decl();
                if self.at_word("from") {
                    self.bump();
                    self.eat(Tok::Str);
                }
                self.terminator();
                return;
            }
        }
        match self.kind() {
            Tok::Import => {
                if exported {
                    self.err("X0013", start, "re-exports are not supported; import from the module that declares the name");
                }
                self.import(start);
            }
            Tok::Function => {
                if let Some(f) = self.function() {
                    let span = start.to(self.prev_span());
                    self.ast.items.push(Item { kind: ItemKind::Function(f), span, exported });
                }
            }
            Tok::Async => {
                let span = self.cur_span();
                self.err("U0003", span, "`async` functions are not supported yet (planned for M6)");
                self.bump();
                if self.at(Tok::Function)
                    && let Some(f) = self.function() {
                        let span = start.to(self.prev_span());
                        self.ast.items.push(Item { kind: ItemKind::Function(f), span, exported });
                    }
            }
            Tok::Type if self.nth(1) == Tok::Ident => self.type_alias(start, exported),
            Tok::Interface => self.interface(start, exported),
            Tok::Const => self.const_item(start, exported),
            Tok::Let | Tok::Var => {
                let kw = self.cur_span();
                let d = if self.at(Tok::Var) {
                    Diagnostic::new("X0004", kw, "`var` is not supported").fix(Applicability::Safe, "use `const`", kw, "const")
                } else {
                    Diagnostic::new("V0201", kw, "mutable module-level state is not allowed")
                        .note("instead", "use `const` for module constants, and keep mutable state inside functions")
                        .fix(Applicability::Maybe, "use `const`", kw, "const")
                };
                self.push(d);
                self.const_item(start, exported);
            }
            Tok::Class | Tok::Abstract => {
                let span = self.cur_span();
                self.err("U0001", span, "classes are not supported yet (planned for M2)");
                self.skip_decl();
            }
            Tok::Enum => {
                let span = self.cur_span();
                self.push(
                    Diagnostic::new("X0005", span, "`enum` is not supported")
                        .note("instead", "use a union of string literals: `type Color = \"red\" | \"green\"`"),
                );
                self.skip_decl();
            }
            Tok::Namespace => {
                let span = self.cur_span();
                self.push(Diagnostic::new("X0006", span, "namespaces are not supported").note("instead", "use one module (file) per namespace"));
                self.skip_decl();
            }
            Tok::Declare => {
                let span = self.cur_span();
                self.err("X0021", span, "`declare` is not supported");
                self.skip_decl();
            }
            Tok::At => {
                let span = self.cur_span();
                self.err("X0015", span, "decorators are not supported");
                self.bump();
                self.ident("a decorator name");
                if self.at(Tok::LParen) {
                    self.skip_balanced();
                }
            }
            Tok::Ident if self.text(self.tok()) == "test" && self.nth(1) == Tok::LParen => self.test_item(start),
            _ => {
                let s = self.stmt();
                let span = self.ast.stmts[s as usize].span;
                if !matches!(self.ast.stmts[s as usize].kind, StmtKind::Error | StmtKind::Empty) {
                    self.push(
                        Diagnostic::new("P0201", span, "statements are not allowed at the top level")
                            .note("instead", "put program logic in `function main() { ... }`; the top level holds only declarations, `const`s and `test(...)` calls"),
                    );
                }
            }
        }
    }

    fn import(&mut self, start: Span) {
        self.bump(); // import
        if self.at(Tok::Type) {
            self.bump(); // `import type { ... }` is the same as `import { ... }`
        }
        let mut names = Vec::new();
        let mut namespace = None;
        if self.at(Tok::Str) {
            let span = self.cur_span();
            self.err("X0031", span, "side-effect imports are not supported; modules have no initialization side effects");
            self.bump();
            self.terminator();
            return;
        } else if self.eat(Tok::Star) {
            if !self.at_word("as") {
                self.expect(Tok::Ident, "(`as`) after `import *`");
            } else {
                self.bump();
            }
            namespace = self.ident("a namespace name");
        } else if self.at(Tok::LBrace) {
            self.bump();
            while !self.at(Tok::RBrace) && !self.at(Tok::Eof) {
                if self.at(Tok::Type) {
                    self.bump();
                }
                let Some(name) = self.ident("an imported name") else {
                    self.recover_line();
                    return;
                };
                names.push(name);
                if self.at_word("as") {
                    let as_span = self.cur_span();
                    self.bump();
                    let alias = self.ident("a name");
                    let span = as_span.to(self.prev_span());
                    let original = self.interner.get(name.0).to_string();
                    let mut d = Diagnostic::new("X0012", span, "renaming imports is not supported")
                        .note("why", "every name has one spelling across the codebase, so search finds all uses");
                    if alias.is_some() {
                        d = d.fix(Applicability::Maybe, format!("import `{original}` without renaming and use that name"), Span::new(self.file, name.1.end, span.end), "");
                    }
                    self.push(d);
                }
                if !self.eat(Tok::Comma) {
                    break;
                }
            }
            self.expect(Tok::RBrace, "to close the import list");
        } else if self.at(Tok::Ident) {
            let span = self.cur_span();
            let name = self.text(self.tok()).to_string();
            self.push(
                Diagnostic::new("X0011", span, "default imports are not supported")
                    .note("instead", format!("import named exports: `import {{ {name} }} from ...` or `import * as {name} from ...`")),
            );
            self.bump();
        } else {
            let span = self.cur_span();
            let msg = format!("expected `{{`, `*` or a string after `import`, found {}", self.found());
            self.err("P0001", span, msg);
            self.recover_line();
            return;
        }
        if !self.at_word("from") {
            let span = self.cur_span();
            let msg = format!("expected `from` after the import list, found {}", self.found());
            self.err("P0001", span, msg);
            self.recover_line();
            return;
        }
        self.bump();
        if !self.at(Tok::Str) {
            let span = self.cur_span();
            let msg = format!("expected a module path string, found {}", self.found());
            self.err("P0001", span, msg);
            self.recover_line();
            return;
        }
        let t = self.bump();
        let path = self.strings[t.val as usize].clone();
        let path_span = self.span_of(t);
        self.terminator();
        let span = start.to(path_span);
        self.ast.items.push(Item { kind: ItemKind::Import(Import { names, namespace, path, path_span }), span, exported: false });
    }

    fn tparams(&mut self) -> Vec<TypeParam> {
        let mut out = Vec::new();
        if !self.eat(Tok::Lt) {
            return out;
        }
        while !self.at(Tok::Gt) && !self.at(Tok::Eof) {
            let Some((name, span)) = self.ident("a type parameter name") else { break };
            let bound = if self.eat(Tok::Extends) { Some(self.ty()) } else { None };
            if self.at(Tok::Eq) {
                let s = self.cur_span();
                self.err("U0005", s, "default type arguments are not supported yet");
                self.bump();
                self.ty();
            }
            out.push(TypeParam { name, span, bound });
            if !self.eat(Tok::Comma) {
                break;
            }
        }
        self.expect(Tok::Gt, "to close the type parameter list");
        out
    }

    fn params(&mut self) -> Vec<Param> {
        let mut out = Vec::new();
        self.expect(Tok::LParen, "to start the parameter list");
        while !self.at(Tok::RParen) && !self.at(Tok::Eof) {
            let start = self.cur_span();
            if self.at(Tok::DotDotDot) {
                self.err("U0011", start, "rest parameters are not supported yet; take an array parameter instead");
                self.bump();
            }
            if self.at(Tok::LBrace) || self.at(Tok::LBracket) {
                self.err("U0006", start, "destructuring parameters are not supported yet; take the value and read its fields");
                self.skip_balanced();
                if self.eat(Tok::Colon) {
                    self.ty();
                }
                if !self.eat(Tok::Comma) {
                    break;
                }
                continue;
            }
            let inout = if self.at_word("inout") && self.nth(1) == Tok::Ident {
                self.bump();
                true
            } else {
                false
            };
            let Some((name, span)) = self.ident("a parameter name") else {
                self.skip_to_paren_end();
                break;
            };
            let optional = self.eat(Tok::Question);
            let ty = if self.eat(Tok::Colon) { Some(self.ty()) } else { None };
            if self.at(Tok::Eq) {
                let s = self.cur_span();
                self.bump();
                let e = self.assign();
                let s = s.to(self.expr_span(e));
                self.err("U0005", s, "default parameter values are not supported yet; make the parameter optional (`x?: T`) and use `x ?? default`");
            }
            out.push(Param { name, span, ty, inout, optional });
            if !self.eat(Tok::Comma) {
                break;
            }
        }
        self.expect(Tok::RParen, "to close the parameter list");
        out
    }

    fn skip_to_paren_end(&mut self) {
        let mut depth = 0i32;
        while !self.at(Tok::Eof) {
            match self.kind() {
                Tok::LParen => depth += 1,
                Tok::RParen => {
                    if depth == 0 {
                        return;
                    }
                    depth -= 1;
                }
                _ => {}
            }
            self.bump();
        }
    }

    fn function(&mut self) -> Option<FnDecl> {
        self.bump(); // function
        if self.at(Tok::Star) {
            let span = self.cur_span();
            self.err("U0013", span, "generator functions are not supported yet");
            self.bump();
        }
        let (name, name_span) = self.ident("a function name")?;
        let tparams = self.tparams();
        let params = self.params();
        let ret = if self.eat(Tok::Colon) { Some(self.ty()) } else { None };
        self.effects_clause();
        if !self.at(Tok::LBrace) {
            let span = self.cur_span();
            let msg = format!("expected `{{` to start the function body, found {}", self.found());
            self.err("P0001", span, msg);
            self.recover_line();
            return None;
        }
        let body = self.block();
        Some(FnDecl { name, name_span, tparams, params, ret, body })
    }

    /// `throws E` / `uses fs | net` (parsed, not yet checked).
    fn effects_clause(&mut self) {
        loop {
            if self.at_word("throws") {
                let span = self.cur_span();
                self.err("U0004", span, "`throws` clauses are not supported yet (planned for M3)");
                self.bump();
                self.ty();
            } else if self.at_word("uses") {
                let span = self.cur_span();
                self.err("U0004", span, "`uses` effect clauses are not supported yet (planned for M3)");
                self.bump();
                self.ident("an effect name");
                while self.eat(Tok::Pipe) {
                    self.ident("an effect name");
                }
            } else {
                return;
            }
        }
    }

    fn type_alias(&mut self, start: Span, exported: bool) {
        self.bump(); // type
        let Some((name, name_span)) = self.ident("a type name") else {
            self.recover_line();
            return;
        };
        let tparams = self.tparams();
        if !self.expect(Tok::Eq, "after the type name") {
            self.recover_line();
            return;
        }
        let ty = self.ty();
        self.terminator();
        let span = start.to(self.prev_span());
        self.ast.items.push(Item { kind: ItemKind::TypeAlias { name, name_span, tparams, ty }, span, exported });
    }

    fn interface(&mut self, start: Span, exported: bool) {
        self.bump(); // interface
        let Some((name, name_span)) = self.ident("an interface name") else {
            self.skip_decl();
            return;
        };
        let tparams = self.tparams();
        if self.at(Tok::Extends) {
            let span = self.cur_span();
            self.err("U0014", span, "interface inheritance is not supported yet; repeat the members");
            self.bump();
            while !self.at(Tok::LBrace) && !self.at(Tok::Eof) {
                self.bump();
            }
        }
        if !self.at(Tok::LBrace) {
            self.expect(Tok::LBrace, "to start the interface body");
            self.skip_decl();
            return;
        }
        let members = self.record_fields();
        let span = start.to(self.prev_span());
        self.ast.items.push(Item { kind: ItemKind::Interface { name, name_span, tparams, members }, span, exported });
    }

    fn const_item(&mut self, start: Span, exported: bool) {
        self.bump(); // const
        if self.at(Tok::LBrace) || self.at(Tok::LBracket) {
            let span = self.cur_span();
            self.err("U0006", span, "destructuring is not supported yet; bind the value, then read its fields");
            self.recover_line();
            return;
        }
        let Some((name, name_span)) = self.ident("a constant name") else {
            self.recover_line();
            return;
        };
        let ty = if self.eat(Tok::Colon) { Some(self.ty()) } else { None };
        if !self.expect(Tok::Eq, "(module constants need a value)") {
            self.recover_line();
            return;
        }
        let init = self.assign();
        if self.at(Tok::Comma) {
            let span = self.cur_span();
            self.err("P0005", span, "declare one constant per statement");
            self.recover_line();
        } else {
            self.terminator();
        }
        let span = start.to(self.prev_span());
        self.ast.items.push(Item { kind: ItemKind::Const { name, name_span, ty, init }, span, exported });
    }

    fn test_item(&mut self, start: Span) {
        self.bump(); // test
        self.bump(); // (
        if !self.at(Tok::Str) {
            let span = self.cur_span();
            self.err("P0001", span, "expected a test name string: `test(\"name\", () => { ... })`");
            self.recover_line();
            return;
        }
        let t = self.bump();
        let name = self.strings[t.val as usize].clone();
        let name_span = self.span_of(t);
        self.expect(Tok::Comma, "after the test name");
        let body = self.assign();
        self.eat(Tok::Comma);
        self.expect(Tok::RParen, "to close `test(...)`");
        self.terminator();
        let span = start.to(self.prev_span());
        self.ast.items.push(Item { kind: ItemKind::Test { name, name_span, body }, span, exported: false });
    }

    // ---------------------------------------------------------------- types

    fn ty(&mut self) -> TypeId {
        let start = self.cur_span();
        self.eat(Tok::Pipe);
        let first = self.postfix_ty();
        if self.at(Tok::Amp) {
            let span = self.cur_span();
            self.push(
                Diagnostic::new("X0014", span, "intersection types (`&`) are not supported")
                    .note("instead", "declare a record type with all the fields"),
            );
            while self.eat(Tok::Amp) {
                self.postfix_ty();
            }
            let span = start.to(self.prev_span());
            return self.mk_ty(TypeExprKind::Error, span);
        }
        if !self.at(Tok::Pipe) {
            return first;
        }
        let mut members = vec![first];
        while self.eat(Tok::Pipe) {
            members.push(self.postfix_ty());
        }
        let span = start.to(self.prev_span());
        self.mk_ty(TypeExprKind::Union(members), span)
    }

    fn postfix_ty(&mut self) -> TypeId {
        let start = self.cur_span();
        let mut t = self.primary_ty();
        while self.at(Tok::LBracket) && self.nth(1) == Tok::RBracket && !self.tok().nl_before {
            self.bump();
            self.bump();
            let span = start.to(self.prev_span());
            t = self.mk_ty(TypeExprKind::Array(t), span);
        }
        t
    }

    fn primary_ty(&mut self) -> TypeId {
        let start = self.cur_span();
        match self.kind() {
            Tok::LParen => {
                if let Some(close) = self.matching_paren()
                    && self.toks.get(close + 1).map(|t| t.kind) == Some(Tok::Arrow) {
                        let params = self.params();
                        self.expect(Tok::Arrow, "in the function type");
                        let ret = self.ty();
                        let span = start.to(self.prev_span());
                        return self.mk_ty(TypeExprKind::Func(params, ret), span);
                    }
                self.bump();
                let t = self.ty();
                self.expect(Tok::RParen, "to close the parenthesized type");
                t
            }
            Tok::LBrace => {
                let fields = self.record_fields();
                let span = start.to(self.prev_span());
                self.mk_ty(TypeExprKind::Record(fields), span)
            }
            Tok::Str => {
                let t = self.bump();
                let s = self.interner.intern(&self.strings[t.val as usize]);
                self.mk_ty(TypeExprKind::StrLit(s), start)
            }
            Tok::Int | Tok::Float | Tok::True | Tok::False => {
                self.bump();
                self.err("U0007", start, "numeric and boolean literal types are not supported yet; use `int`, `f64` or `bool`");
                self.mk_ty(TypeExprKind::Error, start)
            }
            Tok::Undefined => {
                self.bump();
                self.mk_ty(TypeExprKind::Undefined, start)
            }
            Tok::Null => {
                self.bump();
                self.mk_ty(TypeExprKind::Null, start)
            }
            Tok::Void => {
                self.bump();
                self.mk_ty(TypeExprKind::Void, start)
            }
            Tok::Typeof => {
                self.bump();
                self.push(Diagnostic::new("X0016", start, "`typeof` in type position is not supported").note("instead", "name the type explicitly"));
                self.postfix_expr_no_call();
                let span = start.to(self.prev_span());
                self.mk_ty(TypeExprKind::Error, span)
            }
            Tok::LBracket => {
                self.err("U0008", start, "tuple types are not supported yet; use a record type with named fields");
                self.skip_balanced();
                let span = start.to(self.prev_span());
                self.mk_ty(TypeExprKind::Error, span)
            }
            Tok::Ident => {
                let t = self.bump();
                let word = self.text(t);
                if word == "keyof" {
                    self.push(Diagnostic::new("X0016", start, "`keyof` is not supported").note("instead", "use a union of string literals"));
                    self.postfix_ty();
                    let span = start.to(self.prev_span());
                    return self.mk_ty(TypeExprKind::Error, span);
                }
                let mut name = self.interner.intern(word);
                let mut name_span = start;
                let mut ns = None;
                if self.at(Tok::Dot) && self.adjacent() {
                    self.bump();
                    if let Some((n, s)) = self.ident("a type name") {
                        ns = Some((name, name_span));
                        name = n;
                        name_span = s;
                    }
                }
                let mut args = Vec::new();
                if self.at(Tok::Lt) && !self.tok().nl_before {
                    self.bump();
                    while !self.at(Tok::Gt) && !self.at(Tok::Eof) {
                        args.push(self.ty());
                        if !self.eat(Tok::Comma) {
                            break;
                        }
                    }
                    self.expect(Tok::Gt, "to close the type argument list");
                }
                let span = start.to(self.prev_span());
                self.mk_ty(TypeExprKind::Named { ns, name, name_span, args }, span)
            }
            _ => {
                let msg = format!("expected a type, found {}", self.found());
                self.err("P0003", start, msg);
                self.mk_ty(TypeExprKind::Error, start)
            }
        }
    }

    /// `{ a: T, b?: U, m(x: T): R }` for record types and interfaces.
    fn record_fields(&mut self) -> Vec<FieldTy> {
        self.bump(); // {
        let mut fields = Vec::new();
        while !self.at(Tok::RBrace) && !self.at(Tok::Eof) {
            let start = self.cur_span();
            if self.at_word("readonly") && self.nth(1) != Tok::Colon && self.nth(1) != Tok::Question {
                self.bump(); // values are copied, so `readonly` adds nothing; accept it for familiarity
            }
            if self.at(Tok::LBracket) {
                self.push(
                    Diagnostic::new("X0017", start, "index signatures are not supported")
                        .note("instead", "use `Map<K, V>` for dynamic keys"),
                );
                self.skip_balanced();
                if self.eat(Tok::Colon) {
                    self.ty();
                }
            } else if let Some((name, name_span)) = self.prop_name() {
                let optional = self.eat(Tok::Question);
                if self.at(Tok::LParen) || self.at(Tok::Lt) {
                    // Method signature: `area(): f64` is a field of function type.
                    let tps = self.tparams();
                    if !tps.is_empty() {
                        self.err("U0015", start, "generic methods in interfaces are not supported yet");
                    }
                    let params = self.params();
                    self.expect(Tok::Colon, "before the method return type");
                    let ret = self.ty();
                    let span = start.to(self.prev_span());
                    let ty = self.mk_ty(TypeExprKind::Func(params, ret), span);
                    fields.push(FieldTy { name, name_span, optional, ty });
                } else {
                    self.expect(Tok::Colon, "after the field name");
                    let ty = self.ty();
                    fields.push(FieldTy { name, name_span, optional, ty });
                }
            } else {
                let msg = format!("expected a field name, found {}", self.found());
                self.err("P0001", start, msg);
                self.recover_line();
                if self.at(Tok::RBrace) {
                    break;
                }
                continue;
            }
            if !(self.eat(Tok::Comma) || self.eat(Tok::Semi) || self.at(Tok::RBrace) || self.tok().nl_before) {
                let msg = format!("expected `,` between fields, found {}", self.found());
                let span = self.cur_span();
                self.err("P0001", span, msg);
                self.recover_line();
            }
        }
        self.expect(Tok::RBrace, "to close the record type");
        fields
    }

    // ---------------------------------------------------------------- statements

    fn block(&mut self) -> StmtId {
        let start = self.cur_span();
        self.expect(Tok::LBrace, "to start a block");
        let mut stmts = Vec::new();
        while !self.at(Tok::RBrace) && !self.at(Tok::Eof) {
            let before = self.pos;
            stmts.push(self.stmt());
            if self.pos == before {
                self.bump();
            }
        }
        self.expect(Tok::RBrace, "to close the block");
        let span = start.to(self.prev_span());
        self.mk_stmt(StmtKind::Block(stmts), span)
    }

    fn stmt(&mut self) -> StmtId {
        let start = self.cur_span();
        match self.kind() {
            Tok::LBrace => self.block(),
            Tok::Const | Tok::Let | Tok::Var => {
                let s = self.let_stmt();
                self.terminator();
                s
            }
            Tok::If => {
                self.bump();
                self.expect(Tok::LParen, "after `if`");
                let cond = self.expr();
                self.expect(Tok::RParen, "to close the `if` condition");
                let then = self.stmt();
                let els = if self.eat(Tok::Else) { Some(self.stmt()) } else { None };
                let span = start.to(self.prev_span());
                self.mk_stmt(StmtKind::If(cond, then, els), span)
            }
            Tok::While => {
                self.bump();
                self.expect(Tok::LParen, "after `while`");
                let cond = self.expr();
                self.expect(Tok::RParen, "to close the `while` condition");
                let body = self.stmt();
                let span = start.to(self.prev_span());
                self.mk_stmt(StmtKind::While(cond, body), span)
            }
            Tok::Do => {
                self.bump();
                let body = self.stmt();
                self.expect(Tok::While, "after the `do` body");
                self.expect(Tok::LParen, "after `while`");
                let cond = self.expr();
                self.expect(Tok::RParen, "to close the `while` condition");
                self.terminator();
                let span = start.to(self.prev_span());
                self.mk_stmt(StmtKind::DoWhile(body, cond), span)
            }
            Tok::For => self.for_stmt(),
            Tok::Switch => self.switch_stmt(),
            Tok::Return => {
                self.bump();
                let value = if self.at(Tok::Semi) || self.at(Tok::RBrace) || self.at(Tok::Eof) || self.tok().nl_before { None } else { Some(self.expr()) };
                self.terminator();
                let span = start.to(self.prev_span());
                self.mk_stmt(StmtKind::Return(value), span)
            }
            Tok::Break | Tok::Continue => {
                let brk = self.at(Tok::Break);
                self.bump();
                if self.at(Tok::Ident) && !self.tok().nl_before {
                    let span = self.cur_span();
                    self.err("X0018", span, "labels are not supported; restructure the loop or move it into a function and `return`");
                    self.bump();
                }
                self.terminator();
                self.mk_stmt(if brk { StmtKind::Break } else { StmtKind::Continue }, start)
            }
            Tok::Semi => {
                self.bump();
                self.mk_stmt(StmtKind::Empty, start)
            }
            Tok::Throw => {
                self.err("U0004", start, "`throw` is not supported yet (planned for M3)");
                self.bump();
                self.expr();
                self.terminator();
                self.mk_stmt(StmtKind::Error, start)
            }
            Tok::Try => {
                self.err("U0004", start, "`try`/`catch` is not supported yet (planned for M3)");
                self.bump();
                self.skip_balanced();
                while self.at(Tok::Catch) || self.at(Tok::Finally) {
                    self.bump();
                    if self.at(Tok::LParen) {
                        self.skip_balanced();
                    }
                    self.skip_balanced();
                }
                self.mk_stmt(StmtKind::Error, start)
            }
            Tok::Function => {
                self.push(
                    Diagnostic::new("U0010", start, "nested function declarations are not supported yet")
                        .note("instead", "declare it at the top level, or use `const name = (...) => { ... }`"),
                );
                self.skip_decl();
                self.mk_stmt(StmtKind::Error, start)
            }
            Tok::Class | Tok::Enum | Tok::Interface | Tok::Import | Tok::Export => {
                let msg = format!("{} declarations are only allowed at the top level", self.found());
                self.err("P0006", start, msg);
                self.skip_decl();
                self.mk_stmt(StmtKind::Error, start)
            }
            Tok::Type if self.nth(1) == Tok::Ident => {
                self.err("P0006", start, "type declarations are only allowed at the top level");
                self.recover_line();
                self.mk_stmt(StmtKind::Error, start)
            }
            Tok::Ident if self.nth(1) == Tok::Colon => {
                self.err("X0018", start, "labels are not supported");
                self.bump();
                self.bump();
                self.stmt()
            }
            _ => {
                let e = self.expr();
                if self.at(Tok::Comma) {
                    let span = self.cur_span();
                    self.err("X0024", span, "the comma operator is not supported; write separate statements");
                    self.recover_line();
                } else {
                    self.terminator();
                }
                let span = start.to(self.prev_span());
                self.mk_stmt(StmtKind::Expr(e), span)
            }
        }
    }

    /// `const x: T = e` / `let x = e` (without the terminator).
    fn let_stmt(&mut self) -> StmtId {
        let start = self.cur_span();
        let mutable = match self.kind() {
            Tok::Var => {
                self.push(Diagnostic::new("X0004", start, "`var` is not supported").fix(Applicability::Safe, "use `let`", start, "let"));
                true
            }
            Tok::Let => true,
            _ => false,
        };
        self.bump();
        if self.at(Tok::LBrace) || self.at(Tok::LBracket) {
            let span = self.cur_span();
            self.err("U0006", span, "destructuring is not supported yet; bind the value, then read its fields");
            self.recover_line();
            return self.mk_stmt(StmtKind::Error, start);
        }
        let Some((name, name_span)) = self.ident("a variable name") else {
            self.recover_line();
            return self.mk_stmt(StmtKind::Error, start);
        };
        let ty = if self.eat(Tok::Colon) { Some(self.ty()) } else { None };
        let init = if self.eat(Tok::Eq) { Some(self.assign()) } else { None };
        if self.at(Tok::Comma) {
            let span = self.cur_span();
            self.err("P0005", span, "declare one variable per statement");
            self.recover_line();
        }
        let span = start.to(self.prev_span());
        self.mk_stmt(StmtKind::Let { mutable, name, name_span, ty, init }, span)
    }

    fn for_stmt(&mut self) -> StmtId {
        let start = self.cur_span();
        self.bump(); // for
        if self.at(Tok::Await) {
            let span = self.cur_span();
            self.err("U0003", span, "`for await` is not supported yet (planned for M6)");
            self.bump();
        }
        self.expect(Tok::LParen, "after `for`");
        if matches!(self.kind(), Tok::Const | Tok::Let | Tok::Var) && self.nth(1) == Tok::Ident {
            let is_of = self.toks.get(self.pos + 2).map(|t| t.kind == Tok::Ident && self.text(*t) == "of").unwrap_or(false);
            let is_in = self.nth(2) == Tok::In;
            if is_of || is_in {
                let mutable = !self.at(Tok::Const);
                self.bump();
                let (name, name_span) = self.ident("a loop variable").unwrap();
                let kw = self.cur_span();
                self.bump(); // of / in
                if is_in {
                    self.push(
                        Diagnostic::new("X0007", kw, "`for...in` is not supported")
                            .note("instead", "iterate keys with `for (const k of map.keys())`, or values with `for...of`"),
                    );
                }
                let iter = self.expr();
                self.expect(Tok::RParen, "to close the `for` header");
                let body = self.stmt();
                let span = start.to(self.prev_span());
                if is_in {
                    return self.mk_stmt(StmtKind::Error, span);
                }
                return self.mk_stmt(StmtKind::ForOf { mutable, name, name_span, iter, body }, span);
            }
        }
        let init = if self.at(Tok::Semi) {
            None
        } else if matches!(self.kind(), Tok::Const | Tok::Let | Tok::Var) {
            Some(self.let_stmt())
        } else {
            let e = self.expr();
            let span = self.expr_span(e);
            Some(self.mk_stmt(StmtKind::Expr(e), span))
        };
        self.expect(Tok::Semi, "after the `for` initializer");
        let cond = if self.at(Tok::Semi) { None } else { Some(self.expr()) };
        self.expect(Tok::Semi, "after the `for` condition");
        let step = if self.at(Tok::RParen) { None } else { Some(self.expr()) };
        self.expect(Tok::RParen, "to close the `for` header");
        let body = self.stmt();
        let span = start.to(self.prev_span());
        self.mk_stmt(StmtKind::For { init, cond, step, body }, span)
    }

    fn switch_stmt(&mut self) -> StmtId {
        let start = self.cur_span();
        self.bump(); // switch
        self.expect(Tok::LParen, "after `switch`");
        let disc = self.expr();
        self.expect(Tok::RParen, "to close the `switch` value");
        self.expect(Tok::LBrace, "to start the `switch` body");
        let mut cases = Vec::new();
        while !self.at(Tok::RBrace) && !self.at(Tok::Eof) {
            let case_start = self.cur_span();
            let test = if self.eat(Tok::Case) {
                Some(self.expr())
            } else if self.eat(Tok::Default) {
                None
            } else {
                let msg = format!("expected `case` or `default`, found {}", self.found());
                self.err("P0001", case_start, msg);
                self.recover_line();
                continue;
            };
            self.expect(Tok::Colon, "after the case label");
            let mut body = Vec::new();
            while !matches!(self.kind(), Tok::Case | Tok::Default | Tok::RBrace | Tok::Eof) {
                let before = self.pos;
                body.push(self.stmt());
                if self.pos == before {
                    self.bump();
                }
            }
            let span = case_start.to(self.prev_span());
            cases.push(Case { test, body, span });
        }
        self.expect(Tok::RBrace, "to close the `switch` body");
        let span = start.to(self.prev_span());
        self.mk_stmt(StmtKind::Switch(disc, cases), span)
    }

    // ---------------------------------------------------------------- expressions

    fn expr(&mut self) -> ExprId {
        self.assign()
    }

    fn assign_op(&self) -> Option<(AssignOp, usize)> {
        use BinOp::*;
        let op = match self.kind() {
            Tok::Eq => AssignOp::Assign,
            Tok::PlusEq => AssignOp::Op(Add),
            Tok::MinusEq => AssignOp::Op(Sub),
            Tok::StarEq => AssignOp::Op(Mul),
            Tok::SlashEq => AssignOp::Op(Div),
            Tok::PercentEq => AssignOp::Op(Rem),
            Tok::StarStarEq => AssignOp::Op(Pow),
            Tok::AmpEq => AssignOp::Op(BitAnd),
            Tok::PipeEq => AssignOp::Op(BitOr),
            Tok::CaretEq => AssignOp::Op(BitXor),
            Tok::LtLtEq => AssignOp::Op(Shl),
            Tok::QuestionQuestionEq => AssignOp::Op(Nullish),
            Tok::AmpAmpEq => AssignOp::Op(And),
            Tok::PipePipeEq => AssignOp::Op(Or),
            Tok::Gt => {
                // `>>=` and `>>>=` arrive as separate adjacent tokens.
                let t = &self.toks[self.pos..];
                let glued = |i: usize| t.get(i).zip(t.get(i + 1)).map(|(a, b)| a.end == b.start).unwrap_or(false);
                if t.len() > 3 && t[1].kind == Tok::Gt && t[2].kind == Tok::Gt && t[3].kind == Tok::Eq && glued(0) && glued(1) && glued(2) {
                    return Some((AssignOp::Op(UShr), 4));
                }
                if t.len() > 2 && t[1].kind == Tok::Gt && t[2].kind == Tok::Eq && glued(0) && glued(1) {
                    return Some((AssignOp::Op(Shr), 3));
                }
                return None;
            }
            _ => return None,
        };
        Some((op, 1))
    }

    fn assign(&mut self) -> ExprId {
        if let Some(e) = self.try_arrow() {
            return e;
        }
        let lhs = self.cond_expr();
        if let Some((op, n)) = self.assign_op() {
            for _ in 0..n {
                self.bump();
            }
            let rhs = self.assign();
            let span = self.expr_span(lhs).to(self.expr_span(rhs));
            return self.mk_expr(ExprKind::Assign(op, lhs, rhs), span);
        }
        lhs
    }

    fn cond_expr(&mut self) -> ExprId {
        let cond = self.binary(1);
        if !self.at(Tok::Question) {
            return cond;
        }
        self.bump();
        let a = self.assign();
        self.expect(Tok::Colon, "in the conditional expression");
        let b = self.assign();
        let span = self.expr_span(cond).to(self.expr_span(b));
        self.mk_expr(ExprKind::Cond(cond, a, b), span)
    }

    /// Returns (operator, tokens to consume, precedence). `as` is reported as `None` op.
    fn peek_binop(&self) -> Option<(Option<BinOp>, usize, u8)> {
        use BinOp::*;
        let t = &self.toks[self.pos..];
        let glued = |i: usize| t.get(i).zip(t.get(i + 1)).map(|(a, b)| a.end == b.start).unwrap_or(false);
        let (op, n) = match self.kind() {
            Tok::PipePipe => (Or, 1),
            Tok::QuestionQuestion => (Nullish, 1),
            Tok::AmpAmp => (And, 1),
            Tok::Pipe => (BitOr, 1),
            Tok::Caret => (BitXor, 1),
            Tok::Amp => (BitAnd, 1),
            Tok::EqEq => (LooseEq, 1),
            Tok::BangEq => (LooseNe, 1),
            Tok::EqEqEq => (Eq, 1),
            Tok::BangEqEq => (Ne, 1),
            Tok::Lt => (Lt, 1),
            Tok::LtEq => (Le, 1),
            Tok::Gt => {
                if t.len() > 2 && t[1].kind == Tok::Gt && t[2].kind == Tok::Gt && glued(0) && glued(1) {
                    if t.get(3).map(|x| x.kind) == Some(Tok::Eq) && glued(2) {
                        return None; // `>>>=`
                    }
                    (UShr, 3)
                } else if t.len() > 1 && t[1].kind == Tok::Gt && glued(0) {
                    if t.get(2).map(|x| x.kind) == Some(Tok::Eq) && glued(1) {
                        return None; // `>>=`
                    }
                    (Shr, 2)
                } else if t.len() > 1 && t[1].kind == Tok::Eq && glued(0) {
                    (Ge, 2)
                } else {
                    (Gt, 1)
                }
            }
            Tok::LtLt => (Shl, 1),
            Tok::Plus => (Add, 1),
            Tok::Minus => (Sub, 1),
            Tok::Star => (Mul, 1),
            Tok::Slash => (Div, 1),
            Tok::Percent => (Rem, 1),
            Tok::StarStar => (Pow, 1),
            Tok::In => (In, 1),
            Tok::Instanceof => (Instanceof, 1),
            Tok::Ident if self.text(self.tok()) == "as" => return Some((None, 1, 8)),
            Tok::Ident if self.text(self.tok()) == "satisfies" => return Some((None, 1, 8)),
            _ => return None,
        };
        let prec = match op {
            Or | Nullish => 1,
            And => 2,
            BitOr => 3,
            BitXor => 4,
            BitAnd => 5,
            LooseEq | LooseNe | Eq | Ne => 6,
            Lt | Gt | Le | Ge | In | Instanceof => 7,
            Shl | Shr | UShr => 8,
            Add | Sub => 9,
            Mul | Div | Rem => 10,
            Pow => 11,
        };
        // `as` is relational-level in TS; keep it just above relational ops.
        let prec = if prec >= 8 { prec + 1 } else { prec };
        Some((Some(op), n, prec))
    }

    fn binary(&mut self, min_prec: u8) -> ExprId {
        let mut lhs = self.unary();
        while let Some((op, n, prec)) = self.peek_binop() {
            if prec < min_prec {
                break;
            }
            let op_span = self.cur_span();
            let is_satisfies = op.is_none() && self.text(self.tok()) == "satisfies";
            for _ in 0..n {
                self.bump();
            }
            let Some(op) = op else {
                let ty = self.ty();
                let span = self.expr_span(lhs).to(self.prev_span());
                if is_satisfies {
                    self.err("X0033", op_span.to(self.prev_span()), "`satisfies` is not supported; annotate the binding's type instead");
                    continue;
                }
                lhs = self.mk_expr(ExprKind::As(lhs, ty), span);
                continue;
            };
            let rhs = if op == BinOp::Pow { self.binary(prec) } else { self.binary(prec + 1) };
            let span = self.expr_span(lhs).to(self.expr_span(rhs));
            lhs = self.mk_expr(ExprKind::Binary(op, lhs, rhs), span);
        }
        lhs
    }

    fn unary(&mut self) -> ExprId {
        let start = self.cur_span();
        let op = match self.kind() {
            Tok::Bang => UnOp::Not,
            Tok::Minus => UnOp::Neg,
            Tok::Plus => UnOp::Plus,
            Tok::Tilde => UnOp::BitNot,
            Tok::Void => UnOp::Void,
            Tok::Typeof => {
                self.bump();
                let e = self.unary();
                let span = start.to(self.expr_span(e));
                return self.mk_expr(ExprKind::Typeof(e), span);
            }
            Tok::Delete => {
                self.bump();
                self.push(
                    Diagnostic::new("X0008", start, "`delete` is not supported")
                        .note("instead", "records have fixed fields; use `Map` and `map.delete(key)` for dynamic keys"),
                );
                return self.unary();
            }
            Tok::Await => {
                self.bump();
                self.err("U0003", start, "`await` is not supported yet (planned for M6)");
                return self.unary();
            }
            Tok::PlusPlus | Tok::MinusMinus => {
                let inc = self.at(Tok::PlusPlus);
                self.bump();
                let target = self.unary();
                let span = start.to(self.expr_span(target));
                return self.mk_expr(ExprKind::Update { inc, prefix: true, target }, span);
            }
            Tok::Lt => {
                self.push(Diagnostic::new("X0019", start, "angle-bracket type assertions are not supported").note("instead", "use `value as T`"));
                self.bump();
                self.ty();
                self.expect(Tok::Gt, "to close the type assertion");
                return self.unary();
            }
            _ => return self.postfix(),
        };
        self.bump();
        let e = self.unary();
        let span = start.to(self.expr_span(e));
        self.mk_expr(ExprKind::Unary(op, e), span)
    }

    fn postfix_expr_no_call(&mut self) {
        self.primary();
        while self.at(Tok::Dot) {
            self.bump();
            self.prop_name();
        }
    }

    fn postfix(&mut self) -> ExprId {
        let mut e = self.primary();
        loop {
            let start = self.expr_span(e);
            match self.kind() {
                Tok::Dot => {
                    self.bump();
                    if self.at(Tok::Hash) {
                        let span = self.cur_span();
                        self.err("U0001", span, "private `#fields` belong to classes, which are not supported yet (planned for M2)");
                        self.bump();
                    }
                    let Some((name, name_span)) = self.prop_name() else {
                        let msg = format!("expected a property name after `.`, found {}", self.found());
                        let span = self.cur_span();
                        self.err("P0001", span, msg);
                        return e;
                    };
                    let span = start.to(name_span);
                    e = self.mk_expr(ExprKind::Member { obj: e, name, name_span, optional: false }, span);
                }
                Tok::QuestionDot => {
                    self.bump();
                    if self.at(Tok::LParen) {
                        let args = self.args();
                        let span = start.to(self.prev_span());
                        e = self.mk_expr(ExprKind::Call { callee: e, type_args: Vec::new(), args, optional: true }, span);
                    } else if self.eat(Tok::LBracket) {
                        let index = self.expr();
                        self.expect(Tok::RBracket, "to close the index");
                        let span = start.to(self.prev_span());
                        e = self.mk_expr(ExprKind::Index { obj: e, index, optional: true }, span);
                    } else if let Some((name, name_span)) = self.prop_name() {
                        let span = start.to(name_span);
                        e = self.mk_expr(ExprKind::Member { obj: e, name, name_span, optional: true }, span);
                    } else {
                        let msg = format!("expected a property name after `?.`, found {}", self.found());
                        let span = self.cur_span();
                        self.err("P0001", span, msg);
                        return e;
                    }
                }
                Tok::LParen if !self.tok().nl_before => {
                    let args = self.args();
                    let span = start.to(self.prev_span());
                    e = self.mk_expr(ExprKind::Call { callee: e, type_args: Vec::new(), args, optional: false }, span);
                }
                Tok::LBracket if !self.tok().nl_before => {
                    self.bump();
                    let index = self.expr();
                    self.expect(Tok::RBracket, "to close the index");
                    let span = start.to(self.prev_span());
                    e = self.mk_expr(ExprKind::Index { obj: e, index, optional: false }, span);
                }
                Tok::Bang if !self.tok().nl_before => {
                    self.bump();
                    let span = start.to(self.prev_span());
                    e = self.mk_expr(ExprKind::NonNull(e), span);
                }
                Tok::PlusPlus | Tok::MinusMinus if !self.tok().nl_before => {
                    let inc = self.at(Tok::PlusPlus);
                    self.bump();
                    let span = start.to(self.prev_span());
                    e = self.mk_expr(ExprKind::Update { inc, prefix: false, target: e }, span);
                }
                Tok::TemplateFull | Tok::TemplateHead if !self.tok().nl_before => {
                    let span = self.cur_span();
                    self.err("X0022", span, "tagged template literals are not supported");
                    self.primary();
                }
                Tok::Lt if self.adjacent() && matches!(self.ast.exprs[e as usize].kind, ExprKind::Ident(_) | ExprKind::Member { .. }) => {
                    // `f<int>(x)`: explicit type arguments, only when followed by a call.
                    let targs = self.speculate(|p| {
                        p.bump();
                        let mut out = Vec::new();
                        while !p.at(Tok::Gt) && !p.at(Tok::Eof) {
                            out.push(p.ty());
                            if !p.eat(Tok::Comma) {
                                break;
                            }
                        }
                        if !p.eat(Tok::Gt) || !p.at(Tok::LParen) {
                            return None;
                        }
                        Some(out)
                    });
                    let Some(type_args) = targs else { return e };
                    let args = self.args();
                    let span = start.to(self.prev_span());
                    e = self.mk_expr(ExprKind::Call { callee: e, type_args, args, optional: false }, span);
                }
                _ => return e,
            }
        }
    }

    fn args(&mut self) -> Vec<Arg> {
        self.bump(); // (
        let mut out = Vec::new();
        while !self.at(Tok::RParen) && !self.at(Tok::Eof) {
            if self.at(Tok::DotDotDot) {
                let span = self.cur_span();
                self.err("U0011", span, "spread arguments are not supported yet");
                self.bump();
            }
            let by_ref = if self.at(Tok::Amp) {
                let span = self.cur_span();
                self.bump();
                Some(span)
            } else {
                None
            };
            let expr = self.assign();
            out.push(Arg { expr, by_ref });
            if !self.eat(Tok::Comma) {
                break;
            }
        }
        self.expect(Tok::RParen, "to close the argument list");
        out
    }

    fn try_arrow(&mut self) -> Option<ExprId> {
        let start = self.cur_span();
        if self.at(Tok::Async) && matches!(self.nth(1), Tok::LParen | Tok::Ident) {
            self.err("U0003", start, "`async` functions are not supported yet (planned for M6)");
            self.bump();
            return self.try_arrow();
        }
        if self.at(Tok::Ident) && self.nth(1) == Tok::Arrow {
            let t = self.bump();
            let name = self.interner.intern(self.text(t));
            let params = vec![Param { name, span: self.span_of(t), ty: None, inout: false, optional: false }];
            self.bump(); // =>
            return Some(self.arrow_body(start, params, None));
        }
        if self.at(Tok::Lt)
            && self.speculate(|p| {
                p.tparams();
                if p.at(Tok::LParen) { Some(()) } else { None }
            })
            .is_some()
            {
                self.err("U0015", start, "generic arrow functions are not supported yet; declare a top-level generic `function`");
                self.tparams();
            }
        if !self.at(Tok::LParen) {
            return None;
        }
        let close = self.matching_paren()?;
        let after = self.toks.get(close + 1).map(|t| t.kind);
        match after {
            Some(Tok::Arrow) => {
                let params = self.params();
                self.bump(); // =>
                Some(self.arrow_body(start, params, None))
            }
            Some(Tok::Colon) => self.speculate(|p| {
                let params = p.params();
                if !p.eat(Tok::Colon) {
                    return None;
                }
                let ret = p.ty();
                if !p.eat(Tok::Arrow) {
                    return None;
                }
                Some(p.arrow_body(start, params, Some(ret)))
            }),
            _ => None,
        }
    }

    fn arrow_body(&mut self, start: Span, params: Vec<Param>, ret: Option<TypeId>) -> ExprId {
        let body = if self.at(Tok::LBrace) { ArrowBody::Block(self.block()) } else { ArrowBody::Expr(self.assign()) };
        let span = start.to(self.prev_span());
        self.mk_expr(ExprKind::Arrow(Box::new(ArrowFn { params, ret, body })), span)
    }

    fn int_value(&mut self, t: Token) -> u64 {
        let text: String = self.text(t).chars().filter(|&c| c != '_').collect();
        let (digits, radix) = match text.get(..2) {
            Some("0x") | Some("0X") => (&text[2..], 16),
            Some("0b") | Some("0B") => (&text[2..], 2),
            Some("0o") | Some("0O") => (&text[2..], 8),
            _ => (&text[..], 10),
        };
        match u64::from_str_radix(digits, radix) {
            Ok(v) => v,
            Err(_) => {
                let span = self.span_of(t);
                self.err("L0005", span, "integer literal is too large for 64 bits");
                0
            }
        }
    }

    fn primary(&mut self) -> ExprId {
        let start = self.cur_span();
        match self.kind() {
            Tok::Int => {
                let t = self.bump();
                let v = self.int_value(t);
                self.mk_expr(ExprKind::Int(v), start)
            }
            Tok::Float => {
                let t = self.bump();
                let text: String = self.text(t).chars().filter(|&c| c != '_').collect();
                let v = text.parse::<f64>().unwrap_or(0.0);
                self.mk_expr(ExprKind::Float(v), start)
            }
            Tok::Str => {
                let t = self.bump();
                let s = self.interner.intern(&self.strings[t.val as usize]);
                self.mk_expr(ExprKind::Str(s), start)
            }
            Tok::TemplateFull => {
                let t = self.bump();
                let s = self.strings[t.val as usize].clone();
                self.mk_expr(ExprKind::Template(vec![s], Vec::new()), start)
            }
            Tok::TemplateHead => {
                let t = self.bump();
                let mut parts = vec![self.strings[t.val as usize].clone()];
                let mut exprs = Vec::new();
                loop {
                    exprs.push(self.expr());
                    match self.kind() {
                        Tok::TemplateMiddle => {
                            let t = self.bump();
                            parts.push(self.strings[t.val as usize].clone());
                        }
                        Tok::TemplateTail => {
                            let t = self.bump();
                            parts.push(self.strings[t.val as usize].clone());
                            break;
                        }
                        _ => {
                            let msg = format!("expected `}}` to close the template substitution, found {}", self.found());
                            let span = self.cur_span();
                            self.err("P0001", span, msg);
                            parts.push(String::new());
                            break;
                        }
                    }
                }
                let span = start.to(self.prev_span());
                self.mk_expr(ExprKind::Template(parts, exprs), span)
            }
            Tok::True | Tok::False => {
                let v = self.at(Tok::True);
                self.bump();
                self.mk_expr(ExprKind::Bool(v), start)
            }
            Tok::Undefined => {
                self.bump();
                self.mk_expr(ExprKind::Undefined, start)
            }
            Tok::Null => {
                self.bump();
                self.mk_expr(ExprKind::Null, start)
            }
            Tok::Ident => {
                let t = self.bump();
                let sym = self.interner.intern(self.text(t));
                self.mk_expr(ExprKind::Ident(sym), start)
            }
            Tok::LParen => {
                self.bump();
                let e = self.expr();
                if self.at(Tok::Comma) {
                    let span = self.cur_span();
                    self.err("X0024", span, "the comma operator is not supported");
                    self.skip_to_paren_end();
                }
                self.expect(Tok::RParen, "to close the parenthesized expression");
                let span = start.to(self.prev_span());
                self.mk_expr(ExprKind::Paren(e), span)
            }
            Tok::LBracket => {
                self.bump();
                let mut elems = Vec::new();
                while !self.at(Tok::RBracket) && !self.at(Tok::Eof) {
                    if self.at(Tok::Comma) {
                        let span = self.cur_span();
                        self.err("P0007", span, "array holes are not supported");
                        self.bump();
                        continue;
                    }
                    if self.at(Tok::DotDotDot) {
                        let span = self.cur_span();
                        self.err("U0011", span, "spread in array literals is not supported yet; use `concat`");
                        self.bump();
                    }
                    elems.push(self.assign());
                    if !self.eat(Tok::Comma) {
                        break;
                    }
                }
                self.expect(Tok::RBracket, "to close the array literal");
                let span = start.to(self.prev_span());
                self.mk_expr(ExprKind::Array(elems), span)
            }
            Tok::LBrace => self.object_literal(),
            Tok::Function => {
                self.push(
                    Diagnostic::new("U0010", start, "function expressions are not supported")
                        .note("instead", "use an arrow function: `(x: T) => { ... }`"),
                );
                self.bump();
                if self.at(Tok::Ident) {
                    self.bump();
                }
                if self.at(Tok::LParen) {
                    self.skip_balanced();
                }
                if self.eat(Tok::Colon) {
                    self.ty();
                }
                if self.at(Tok::LBrace) {
                    self.skip_balanced();
                }
                let span = start.to(self.prev_span());
                self.mk_expr(ExprKind::Error, span)
            }
            Tok::New => {
                self.bump();
                let callee = if let Some((name, span)) = self.ident("a type name after `new`") {
                    self.mk_expr(ExprKind::Ident(name), span)
                } else {
                    return self.mk_expr(ExprKind::Error, start);
                };
                let mut type_args = Vec::new();
                if self.at(Tok::Lt) {
                    self.bump();
                    while !self.at(Tok::Gt) && !self.at(Tok::Eof) {
                        type_args.push(self.ty());
                        if !self.eat(Tok::Comma) {
                            break;
                        }
                    }
                    self.expect(Tok::Gt, "to close the type argument list");
                }
                let args = if self.at(Tok::LParen) { self.args() } else { Vec::new() };
                let span = start.to(self.prev_span());
                self.mk_expr(ExprKind::New { callee, type_args, args }, span)
            }
            Tok::This | Tok::Super | Tok::Class => {
                let msg = format!("{} belongs to classes, which are not supported yet (planned for M2)", self.found());
                self.err("U0001", start, msg);
                self.bump();
                self.mk_expr(ExprKind::Error, start)
            }
            Tok::Slash | Tok::SlashEq => {
                self.err("U0016", start, "regular expression literals are not supported yet");
                self.bump();
                while !self.at(Tok::Eof) && !self.tok().nl_before && !self.at(Tok::Slash) {
                    self.bump();
                }
                self.eat(Tok::Slash);
                if self.at(Tok::Ident) && self.adjacent() {
                    self.bump();
                }
                let span = start.to(self.prev_span());
                self.mk_expr(ExprKind::Error, span)
            }
            _ => {
                let msg = format!("expected an expression, found {}", self.found());
                self.err("P0004", start, msg);
                if !matches!(self.kind(), Tok::RParen | Tok::RBrace | Tok::RBracket | Tok::Semi | Tok::Eof | Tok::Comma) {
                    self.bump();
                }
                self.mk_expr(ExprKind::Error, start)
            }
        }
    }

    fn object_literal(&mut self) -> ExprId {
        let start = self.cur_span();
        self.bump(); // {
        let mut fields = Vec::new();
        while !self.at(Tok::RBrace) && !self.at(Tok::Eof) {
            let fstart = self.cur_span();
            if self.at(Tok::DotDotDot) {
                self.err("U0011", fstart, "object spread is not supported yet; list the fields");
                self.bump();
                self.assign();
            } else if self.at(Tok::Str) || self.at(Tok::Int) || self.at(Tok::LBracket) {
                self.push(
                    Diagnostic::new("X0017", fstart, "string, number and computed keys are not supported in object literals")
                        .note("instead", "records have identifier field names; use `Map<K, V>` for dynamic keys"),
                );
                if self.at(Tok::LBracket) {
                    self.skip_balanced();
                } else {
                    self.bump();
                }
                if self.eat(Tok::Colon) {
                    self.assign();
                }
            } else if let Some((name, name_span)) = self.prop_name() {
                if self.at(Tok::LParen) {
                    self.err("U0012", fstart, "methods in object literals are not supported yet; use a field holding an arrow function");
                    self.skip_balanced();
                    if self.eat(Tok::Colon) {
                        self.ty();
                    }
                    if self.at(Tok::LBrace) {
                        self.skip_balanced();
                    }
                } else if self.eat(Tok::Colon) {
                    let value = self.assign();
                    fields.push(ObjField { name, name_span, value });
                } else {
                    // Shorthand `{ r }`.
                    let value = self.mk_expr(ExprKind::Ident(name), name_span);
                    fields.push(ObjField { name, name_span, value });
                }
            } else {
                let msg = format!("expected a field name, found {}", self.found());
                self.err("P0001", fstart, msg);
                self.recover_line();
                if self.at(Tok::RBrace) {
                    break;
                }
                continue;
            }
            if !self.eat(Tok::Comma) {
                if !self.at(Tok::RBrace) {
                    let msg = format!("expected `,` or `}}` in the object literal, found {}", self.found());
                    let span = self.cur_span();
                    self.err("P0001", span, msg);
                }
                break;
            }
        }
        self.expect(Tok::RBrace, "to close the object literal");
        let span = start.to(self.prev_span());
        self.mk_expr(ExprKind::Object(fields), span)
    }
}
