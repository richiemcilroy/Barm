//! A JavaScript parser for ES modules: it reads the whole grammar to track scopes, and reports
//! what the ESM → CommonJS rewrite needs (import and export declarations, and every reference
//! that resolves to an imported binding). It builds no AST.

use super::lex::{self, Kind, Tok};
use crate::hash::FxMap;

#[derive(Debug)]
pub struct ParseError {
    pub pos: u32,
    pub message: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ImportName {
    Default,
    Namespace,
    Named(String),
}

#[derive(Clone, Debug)]
pub struct ImportBinding {
    pub local: String,
    pub imported: ImportName,
    /// Index into `Module::sources`.
    pub source: usize,
}

#[derive(Clone, Debug)]
pub enum ExportTarget {
    Local(String),
    Import { source: usize, name: ImportName },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ctx {
    Plain,
    /// `{ x }` in an object literal.
    Shorthand,
    /// `x(...)` or ``x`...` ``: called without a receiver.
    Callee,
}

/// A reference to an imported binding, to be rewritten.
#[derive(Clone, Debug)]
pub struct ImportRef {
    pub start: u32,
    pub end: u32,
    pub binding: usize,
    pub ctx: Ctx,
}

#[derive(Default, Debug)]
pub struct Module {
    /// Module specifiers in order of first appearance (imports, re-exports, `export *`).
    pub sources: Vec<String>,
    pub bindings: Vec<ImportBinding>,
    /// Byte ranges to delete.
    pub removals: Vec<(u32, u32)>,
    /// Byte ranges to replace.
    pub replacements: Vec<(u32, u32, String)>,
    /// Text to insert at a byte offset.
    pub inserts: Vec<(u32, String)>,
    /// (exported name, what it refers to), in declaration order.
    pub exports: Vec<(String, ExportTarget)>,
    /// `export * from` sources.
    pub stars: Vec<usize>,
    pub refs: Vec<ImportRef>,
    /// `this` outside any function: `undefined` in a module.
    pub top_this: Vec<(u32, u32)>,
    /// `import.meta` spans.
    pub import_meta: Vec<(u32, u32)>,
    /// Starts of `import(` dynamic imports (the `import` keyword).
    pub dynamic_imports: Vec<(u32, u32)>,
    /// String literals passed to `require(...)` and `import(...)`.
    pub requires: Vec<String>,
    pub top_level_await: Option<u32>,
    /// Per source: whether it is required (TypeScript drops imports used only as types).
    pub used: Vec<bool>,
}

const RESERVED: &[&str] = &[
    "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else", "export", "extends",
    "finally", "for", "function", "if", "import", "in", "instanceof", "new", "return", "super", "switch", "this", "throw", "try",
    "typeof", "var", "void", "while", "with", "null", "true", "false", "enum",
];

#[derive(Clone, Copy, PartialEq, Eq)]
enum ScopeKind {
    Module,
    Function,
    Block,
}

struct Scope {
    parent: u32,
    kind: ScopeKind,
    names: FxMap<String, ()>,
}

struct RawRef {
    start: u32,
    end: u32,
    scope: u32,
    ctx: Ctx,
}

const NONE: u32 = u32::MAX;

struct P<'a> {
    src: &'a str,
    toks: Vec<Tok>,
    i: usize,
    scopes: Vec<Scope>,
    cur: u32,
    refs: Vec<RawRef>,
    m: Module,
    /// Nesting of non-arrow functions (0: module top level, where `this` is undefined).
    fn_depth: u32,
    /// Inside an async function (or the module top level: `await` is top-level await).
    in_async: bool,
    in_generator: bool,
    /// `in` is not an operator here (a `for` initializer).
    no_in: bool,
    /// Import bindings by local name (module scope).
    import_index: FxMap<String, usize>,
    /// TypeScript: types are skipped and recorded as removals.
    ts: bool,
    /// JSX syntax (`.jsx`, `.tsx`).
    jsx: bool,
    /// The module JSX compiles to calls of (`react/jsx-runtime`), and its source index once used.
    jsx_runtime: String,
    jsx_source: Option<usize>,
    /// Corrections of the lexer's regex/division guesses (byte offset, regex), sorted.
    slashes: Vec<(u32, bool)>,
    /// `>>`-style tokens split where type arguments end (byte offset, bytes of the first part).
    splits: Vec<(u32, u32)>,
    /// The import declaration being read (for TypeScript's elision of unused imports).
    import_decls: Vec<ImportDecl>,
    /// Parameter properties (`constructor(private x)`) of the parameter list just read.
    param_props: Vec<String>,
    /// The function just read was a TypeScript signature without a body.
    no_body: bool,
    /// The next `function_rest` is a class constructor.
    ctor_next: bool,
}

/// An import declaration: its source and removal-free flag (`import "x"` always runs).
struct ImportDecl {
    source: usize,
    side_effect: bool,
}

pub fn parse_module(src: &str) -> Result<Module, ParseError> {
    parse(src, false, None)
}

/// Parses a module; `ts`: TypeScript, whose types are recorded as removals; `jsx`: with JSX,
/// compiled for this runtime module (`react/jsx-runtime`).
pub fn parse(src: &str, ts: bool, jsx: Option<&str>) -> Result<Module, ParseError> {
    let opts = lex::LexOpts { jsx: jsx.is_some(), slashes: &[] };
    let toks = lex::tokenize_with(src, &opts).map_err(|e| ParseError { pos: e.pos, message: e.message.to_string() })?;
    let mut p = P {
        src,
        toks,
        i: 0,
        scopes: vec![Scope { parent: NONE, kind: ScopeKind::Module, names: FxMap::default() }],
        cur: 0,
        refs: Vec::new(),
        m: Module::default(),
        fn_depth: 0,
        in_async: true,
        in_generator: false,
        no_in: false,
        import_index: FxMap::default(),
        ts,
        jsx: jsx.is_some(),
        jsx_runtime: jsx.unwrap_or("react/jsx-runtime").to_string(),
        jsx_source: None,
        slashes: Vec::new(),
        splits: Vec::new(),
        import_decls: Vec::new(),
        param_props: Vec::new(),
        no_body: false,
        ctor_next: false,
    };
    while !p.eof() {
        p.statement(true)?;
    }
    p.finish();
    Ok(p.m)
}

type R<T> = Result<T, ParseError>;

impl<'a> P<'a> {
    // ------------------------------------------------------------ tokens

    fn eof(&self) -> bool {
        self.i >= self.toks.len()
    }

    fn tok(&self, k: usize) -> Option<&Tok> {
        self.toks.get(self.i + k)
    }

    fn text(&self, k: usize) -> &'a str {
        match self.toks.get(self.i + k) {
            Some(t) => &self.src[t.start as usize..t.end as usize],
            None => "",
        }
    }

    fn kind(&self, k: usize) -> Option<Kind> {
        self.tok(k).map(|t| t.kind)
    }

    fn is(&self, s: &str) -> bool {
        matches!(self.kind(0), Some(Kind::Punct) | Some(Kind::Ident)) && self.text(0) == s
    }

    fn is_punct(&self, k: usize, s: &str) -> bool {
        self.kind(k) == Some(Kind::Punct) && self.text(k) == s
    }

    fn is_word(&self, k: usize, s: &str) -> bool {
        self.kind(k) == Some(Kind::Ident) && self.text(k) == s
    }

    fn nl_before(&self, k: usize) -> bool {
        self.tok(k).is_some_and(|t| t.nl_before)
    }

    fn pos(&self) -> u32 {
        self.tok(0).map(|t| t.start).unwrap_or(self.src.len() as u32)
    }

    fn prev_end(&self) -> u32 {
        if self.i == 0 { 0 } else { self.toks[self.i - 1].end }
    }

    fn err<T>(&self, message: impl Into<String>) -> R<T> {
        let found = if self.eof() { "end of file".to_string() } else { format!("`{}`", self.text(0)) };
        Err(ParseError { pos: self.pos(), message: format!("{}, found {found}", message.into()) })
    }

    fn bump(&mut self) {
        self.i += 1;
    }

    fn eat(&mut self, s: &str) -> bool {
        if self.is(s) {
            self.bump();
            true
        } else {
            false
        }
    }

    fn expect(&mut self, s: &str) -> R<()> {
        if self.eat(s) { Ok(()) } else { self.err(format!("expected `{s}`")) }
    }

    /// A statement ends: `;`, or before `}` / end / a line break (ASI).
    fn semi(&mut self) -> R<()> {
        if self.eat(";") || self.is_punct(0, "}") || self.eof() || self.nl_before(0) {
            return Ok(());
        }
        self.err("expected `;`")
    }

    /// An identifier (not a reserved word) at token `k`.
    fn is_ident(&self, k: usize) -> bool {
        self.kind(k) == Some(Kind::Ident) && !RESERVED.contains(&self.text(k))
    }

    /// Re-reads the current token: `/` was guessed wrong (`regex`: read a regular expression).
    /// The file is tokenized again with the correction (the lexer's state there — templates, JSX
    /// — comes out right); the tokens before it don't change.
    fn relex(&mut self, regex: bool) -> R<()> {
        let at = self.toks[self.i].start;
        match self.slashes.binary_search_by_key(&at, |s| s.0) {
            Ok(k) => self.slashes[k].1 = regex,
            Err(k) => self.slashes.insert(k, (at, regex)),
        }
        let opts = lex::LexOpts { jsx: self.jsx, slashes: &self.slashes };
        let mut toks = lex::tokenize_with(self.src, &opts).map_err(|e| ParseError { pos: e.pos, message: e.message.to_string() })?;
        // re-apply the splits of `>>`-style tokens
        if !self.splits.is_empty() {
            let mut out = Vec::with_capacity(toks.len() + self.splits.len());
            for t in toks {
                match self.splits.iter().find(|s| s.0 == t.start) {
                    Some(&(start, take)) => {
                        out.push(Tok { kind: Kind::Punct, start, end: start + take, nl_before: t.nl_before });
                        out.push(Tok { kind: Kind::Punct, start: start + take, end: t.end, nl_before: false });
                    }
                    None => out.push(t),
                }
            }
            toks = out;
        }
        self.toks = toks;
        Ok(())
    }

    // ------------------------------------------------------------ scopes

    fn push_scope(&mut self, kind: ScopeKind) -> u32 {
        let id = self.scopes.len() as u32;
        self.scopes.push(Scope { parent: self.cur, kind, names: FxMap::default() });
        self.cur = id;
        id
    }

    fn pop_scope(&mut self) {
        self.cur = self.scopes[self.cur as usize].parent;
    }

    fn declare(&mut self, name: &str, var: bool) {
        let mut s = self.cur;
        if var {
            while self.scopes[s as usize].kind == ScopeKind::Block {
                s = self.scopes[s as usize].parent;
            }
        }
        self.scopes[s as usize].names.insert(name.to_string(), ());
    }

    fn reference(&mut self, k: usize, ctx: Ctx) {
        let t = self.toks[self.i + k];
        if t.text(self.src) == "this" {
            if self.fn_depth == 0 {
                self.m.top_this.push((t.start, t.end));
            }
            return;
        }
        self.refs.push(RawRef { start: t.start, end: t.end, scope: self.cur, ctx });
    }

    fn add_source(&mut self, lit: &str) -> usize {
        let spec = super::bundle::unquote(lit);
        if let Some(i) = self.m.sources.iter().position(|s| *s == spec) {
            return i;
        }
        self.m.sources.push(spec);
        self.m.sources.len() - 1
    }

    /// Resolves the references to imported bindings and the exports of local names.
    fn finish(&mut self) {
        self.m.used = vec![!self.ts; self.m.sources.len()];
        // exports of names with no value at module level: TypeScript types (or nothing)
        let module_names = &self.scopes[0].names;
        let imports = &self.import_index;
        self.m.exports.retain(|(_, t)| match t {
            ExportTarget::Local(n) => module_names.contains_key(n) || imports.contains_key(n),
            ExportTarget::Import { .. } => true,
        });
        let mut seen = FxMap::default();
        self.m.exports.retain(|(n, _)| seen.insert(n.clone(), ()).is_none());
        for (_, t) in &self.m.exports {
            if let ExportTarget::Import { source, .. } = t {
                self.m.used[*source] = true;
            }
        }
        for &s in &self.m.stars {
            self.m.used[s] = true;
        }
        for d in &self.import_decls {
            if d.side_effect {
                self.m.used[d.source] = true;
            }
        }
        if self.import_index.is_empty() {
            return;
        }
        let mut refs = Vec::new();
        for r in &self.refs {
            let name = &self.src[r.start as usize..r.end as usize];
            let Some(&binding) = self.import_index.get(name) else { continue };
            // the nearest declaring scope must be the module scope
            let mut s = r.scope;
            let mut shadowed = false;
            while s != 0 && s != NONE {
                if self.scopes[s as usize].names.contains_key(name) {
                    shadowed = true;
                    break;
                }
                s = self.scopes[s as usize].parent;
            }
            if !shadowed {
                refs.push(ImportRef { start: r.start, end: r.end, binding, ctx: r.ctx });
                let source = self.m.bindings[binding].source;
                self.m.used[source] = true;
            }
        }
        self.m.refs = refs;
        for (_, target) in self.m.exports.iter_mut() {
            if let ExportTarget::Local(name) = target
                && let Some(&b) = self.import_index.get(name.as_str())
            {
                let ib = &self.m.bindings[b];
                self.m.used[ib.source] = true;
                *target = ExportTarget::Import { source: ib.source, name: ib.imported.clone() };
            }
        }
    }

    // ------------------------------------------------------------ statements

    fn statement(&mut self, top: bool) -> R<()> {
        if self.eof() {
            return self.err("expected a statement");
        }
        let kind = self.kind(0).unwrap();
        if kind == Kind::Punct {
            match self.text(0) {
                "{" => return self.block(),
                ";" => {
                    self.bump();
                    return Ok(());
                }
                "@" => {
                    self.decorators()?;
                    return self.statement(top);
                }
                _ => {}
            }
        }
        if kind == Kind::Ident {
            if self.ts_declaration()?.is_some() {
                return Ok(());
            }
            let word = self.text(0);
            match word {
                "var" => return self.var_statement(true).map(drop),
                "const" => return self.var_statement(false).map(drop),
                "let" if self.is_ident(1) || self.is_punct(1, "[") || self.is_punct(1, "{") => return self.var_statement(false).map(drop),
                "function" => return self.function_decl(false),
                "async" if self.is_word(1, "function") && !self.nl_before(1) => {
                    self.bump();
                    return self.function_decl(true);
                }
                "class" => return self.class(true).map(drop),
                "if" => {
                    self.bump();
                    self.paren_expr()?;
                    self.sub_statement()?;
                    if self.eat("else") {
                        self.sub_statement()?;
                    }
                    return Ok(());
                }
                "for" => return self.for_statement(),
                "while" => {
                    self.bump();
                    self.paren_expr()?;
                    return self.sub_statement();
                }
                "do" => {
                    self.bump();
                    self.sub_statement()?;
                    self.expect("while")?;
                    self.paren_expr()?;
                    self.eat(";");
                    return Ok(());
                }
                "return" => {
                    self.bump();
                    if !self.is_punct(0, ";") && !self.is_punct(0, "}") && !self.eof() && !self.nl_before(0) {
                        self.expression()?;
                    }
                    return self.semi();
                }
                "throw" => {
                    self.bump();
                    self.expression()?;
                    return self.semi();
                }
                "break" | "continue" => {
                    self.bump();
                    if self.is_ident(0) && !self.nl_before(0) {
                        self.bump();
                    }
                    return self.semi();
                }
                "try" => return self.try_statement(),
                "switch" => return self.switch_statement(),
                "debugger" => {
                    self.bump();
                    return self.semi();
                }
                "with" => {
                    self.bump();
                    self.paren_expr()?;
                    return self.sub_statement();
                }
                "import" if top && !self.is_punct(1, "(") && !self.is_punct(1, ".") => return self.import_decl(),
                "export" if top => return self.export_decl(),
                _ => {}
            }
            // label
            if self.is_ident(0) && self.is_punct(1, ":") {
                self.bump();
                self.bump();
                return self.statement(false);
            }
        }
        self.expression()?;
        self.semi()
    }

    /// The body of `if`/`for`/`while`/...: a statement (function declarations there are
    /// block-scoped).
    fn sub_statement(&mut self) -> R<()> {
        self.push_scope(ScopeKind::Block);
        let r = self.statement(false);
        self.pop_scope();
        r
    }

    fn block(&mut self) -> R<()> {
        self.expect("{")?;
        self.push_scope(ScopeKind::Block);
        while !self.is_punct(0, "}") {
            if self.eof() {
                return self.err("expected `}`");
            }
            self.statement(false)?;
        }
        self.bump();
        self.pop_scope();
        Ok(())
    }

    fn paren_expr(&mut self) -> R<()> {
        self.expect("(")?;
        self.expression()?;
        self.expect(")")
    }

    fn var_statement(&mut self, var: bool) -> R<Vec<String>> {
        self.bump();
        let names = self.declarations(var)?;
        self.semi()?;
        Ok(names)
    }

    /// `a = 1, {b} = c, ...` after `var`/`let`/`const`; returns the declared names.
    fn declarations(&mut self, var: bool) -> R<Vec<String>> {
        let mut names = Vec::new();
        loop {
            self.binding(var, &mut names)?;
            self.type_annotation()?;
            if self.eat("=") {
                self.assign()?;
            }
            if !self.eat(",") {
                break;
            }
        }
        Ok(names)
    }

    /// A binding pattern: declares its names (into the function scope if `var`).
    fn binding(&mut self, var: bool, names: &mut Vec<String>) -> R<()> {
        if self.is_punct(0, "[") {
            self.bump();
            while !self.is_punct(0, "]") {
                if self.eat(",") {
                    continue;
                }
                self.eat("...");
                self.binding(var, names)?;
                if self.eat("=") {
                    self.assign()?;
                }
                if !self.is_punct(0, "]") {
                    self.expect(",")?;
                }
            }
            self.bump();
            return Ok(());
        }
        if self.is_punct(0, "{") {
            self.bump();
            while !self.is_punct(0, "}") {
                if self.eat("...") {
                    self.binding(var, names)?;
                } else {
                    // key: ident, string, number or [computed]
                    let shorthand = self.is_ident(0) && (self.is_punct(1, ",") || self.is_punct(1, "}") || self.is_punct(1, "="));
                    if shorthand {
                        self.binding(var, names)?;
                    } else {
                        self.property_key()?;
                        self.expect(":")?;
                        self.binding(var, names)?;
                    }
                    if self.eat("=") {
                        self.assign()?;
                    }
                }
                if !self.is_punct(0, "}") {
                    self.expect(",")?;
                }
            }
            self.bump();
            return Ok(());
        }
        if self.kind(0) == Some(Kind::Ident) {
            let name = self.text(0).to_string();
            self.declare(&name, var);
            names.push(name);
            self.bump();
            return Ok(());
        }
        self.err("expected a binding name or pattern")
    }

    fn function_decl(&mut self, is_async: bool) -> R<()> {
        let start = if is_async && self.i > 0 { self.toks[self.i - 1].start } else { self.pos() };
        self.expect("function")?;
        let generator = self.eat("*");
        if self.kind(0) != Some(Kind::Ident) {
            return self.err("expected a function name");
        }
        let name = self.text(0).to_string();
        // function declarations are hoisted to the function scope at a function's top level,
        // block-scoped inside blocks (modules are strict)
        self.declare(&name, false);
        self.bump();
        self.type_params()?;
        self.function_rest(is_async, generator, false)?;
        if self.no_body {
            // a TypeScript overload signature
            self.semi()?;
            self.strip(start);
        }
        Ok(())
    }

    /// Parameters and body (after the name). `arrow`: `this` comes from outside. In TypeScript
    /// a signature may have no body: then `self.no_body` is set.
    fn function_rest(&mut self, is_async: bool, generator: bool, arrow: bool) -> R<()> {
        let ctor = std::mem::take(&mut self.ctor_next);
        let saved = (self.in_async, self.in_generator, self.fn_depth);
        let saved_props = std::mem::take(&mut self.param_props);
        self.in_async = is_async;
        self.in_generator = generator;
        if !arrow {
            self.fn_depth += 1;
        }
        self.push_scope(ScopeKind::Function);
        self.params()?;
        let props = std::mem::replace(&mut self.param_props, saved_props);
        self.return_type()?;
        self.no_body = self.ts && !self.is_punct(0, "{");
        if !self.no_body {
            if ctor && !props.is_empty() {
                self.ctor_body(&props)?;
            } else {
                self.function_body()?;
            }
        }
        self.pop_scope();
        (self.in_async, self.in_generator, self.fn_depth) = saved;
        Ok(())
    }

    /// Strips a TypeScript return type.
    fn return_type(&mut self) -> R<()> {
        if self.ts && self.is_punct(0, ":") {
            let start = self.pos();
            self.bump();
            self.ty()?;
            self.strip(start);
        }
        Ok(())
    }

    fn params(&mut self) -> R<()> {
        self.expect("(")?;
        let mut names = Vec::new();
        while !self.is_punct(0, ")") {
            self.decorators()?;
            if self.ts && self.is_word(0, "this") && (self.is_punct(1, ":") || self.is_punct(1, ",") || self.is_punct(1, ")")) {
                // a `this` parameter: types only
                let start = self.pos();
                self.bump();
                self.type_annotation()?;
                self.eat(",");
                self.strip(start);
                continue;
            }
            let mut prop = false;
            while self.ts
                && matches!(self.text(0), "public" | "private" | "protected" | "readonly" | "override")
                && self.kind(0) == Some(Kind::Ident)
                && (self.kind(1) == Some(Kind::Ident) || self.is_punct(1, "[") || self.is_punct(1, "{"))
            {
                let t = self.toks[self.i];
                self.m.removals.push((t.start, t.end));
                self.bump();
                prop = true;
            }
            self.eat("...");
            if prop && self.kind(0) == Some(Kind::Ident) {
                self.param_props.push(self.text(0).to_string());
            }
            self.binding(true, &mut names)?;
            self.type_annotation()?;
            if self.eat("=") {
                self.assign()?;
            }
            if !self.is_punct(0, ")") {
                self.expect(",")?;
            }
        }
        self.bump();
        Ok(())
    }

    fn function_body(&mut self) -> R<()> {
        self.expect("{")?;
        while !self.is_punct(0, "}") {
            if self.eof() {
                return self.err("expected `}`");
            }
            self.statement(false)?;
        }
        self.bump();
        Ok(())
    }

    /// A constructor body with TypeScript parameter properties: `this.x = x;` goes first, or
    /// after the `super(...)` call.
    fn ctor_body(&mut self, props: &[String]) -> R<()> {
        self.expect("{")?;
        let mut at = self.prev_end();
        let mut after_super = false;
        while !self.is_punct(0, "}") {
            if self.eof() {
                return self.err("expected `}`");
            }
            let is_super = self.is_word(0, "super") && self.is_punct(1, "(");
            self.statement(false)?;
            if is_super && !after_super {
                after_super = true;
                at = self.prev_end();
            }
        }
        self.bump();
        let text: String = props.iter().map(|p| format!(" this.{p} = {p};")).collect();
        self.m.inserts.push((at, text));
        Ok(())
    }

    /// `class [Name] [extends X] { ... }`; returns the class name.
    fn class(&mut self, decl: bool) -> R<Option<String>> {
        self.expect("class")?;
        let mut name = None;
        if self.is_ident(0) && !self.is_word(0, "extends") {
            let n = self.text(0).to_string();
            if decl {
                self.declare(&n, false);
            }
            name = Some(n);
            self.bump();
        }
        // the class name is visible inside the class
        self.push_scope(ScopeKind::Block);
        if let Some(n) = &name {
            let n = n.clone();
            self.declare(&n, false);
        }
        self.type_params()?;
        if self.eat("extends") {
            self.lhs(false)?;
            self.type_params()?;
        }
        if self.ts && self.is_word(0, "implements") {
            let start = self.pos();
            while !self.is_punct(0, "{") && !self.eof() {
                if self.is_punct(0, "<") {
                    self.skip_angles(true)?;
                } else {
                    self.bump();
                }
            }
            self.strip(start);
        }
        self.expect("{")?;
        while !self.is_punct(0, "}") {
            if self.eof() {
                return self.err("expected `}`");
            }
            if self.eat(";") {
                continue;
            }
            self.decorators()?;
            if self.is_word(0, "static") && self.is_punct(1, "{") {
                self.bump();
                let saved = self.fn_depth;
                self.fn_depth += 1;
                self.push_scope(ScopeKind::Function);
                self.function_body()?;
                self.pop_scope();
                self.fn_depth = saved;
                continue;
            }
            self.member(true)?;
        }
        self.bump();
        self.pop_scope();
        Ok(name)
    }

    /// A class member or object literal method/property (after modifiers).
    fn member(&mut self, class: bool) -> R<()> {
        let start = self.pos();
        let mut is_async = false;
        let mut generator = false;
        let mut ambient = false;
        // modifiers: static, accessor, async, get, set (each only if a key follows)
        loop {
            if class {
                ambient |= self.ts_modifiers();
            }
            let w = self.text(0);
            let next_is_key = !(self.is_punct(1, "(") || self.is_punct(1, "=") || self.is_punct(1, ";") || self.is_punct(1, "}") || self.is_punct(1, ":") || self.is_punct(1, ",") || self.is_punct(1, "?") || self.is_punct(1, "!") || self.is_punct(1, "<"));
            if self.kind(0) == Some(Kind::Ident) && next_is_key && !self.nl_before(1) && matches!(w, "static" | "accessor" | "get" | "set" | "async") && class {
                if w == "async" {
                    is_async = true;
                }
                self.bump();
                continue;
            }
            if !class && self.kind(0) == Some(Kind::Ident) && next_is_key && matches!(w, "get" | "set" | "async") && !self.nl_before(1) {
                if w == "async" {
                    is_async = true;
                }
                self.bump();
                continue;
            }
            break;
        }
        // an index signature: `[key: string]: T;`
        if class && self.ts && self.is_punct(0, "[") && self.is_ident(1) && self.is_punct(2, ":") {
            self.skip_balanced()?;
            self.type_annotation()?;
            self.semi()?;
            self.strip(start);
            return Ok(());
        }
        if self.eat("*") {
            generator = true;
        }
        let is_ctor = class && self.text(0) == "constructor";
        self.property_key()?;
        if self.ts && (self.is_punct(0, "?") || self.is_punct(0, "!")) {
            let t = self.toks[self.i];
            self.m.removals.push((t.start, t.end));
            self.bump();
        }
        self.type_params()?;
        if self.is_punct(0, "(") {
            self.ctor_next = is_ctor;
            self.function_rest(is_async, generator, false)?;
            if self.no_body || ambient {
                // an overload signature or abstract method
                self.semi()?;
                self.strip(start);
            }
            return Ok(());
        }
        if class {
            self.type_annotation()?;
            if ambient {
                if self.eat("=") {
                    self.assign()?;
                }
                self.semi()?;
                self.strip(start);
                return Ok(());
            }
            // field, with an optional initializer (evaluated like a method body: `this` is the instance)
            if self.eat("=") {
                let saved = self.fn_depth;
                self.fn_depth += 1;
                self.push_scope(ScopeKind::Function);
                self.assign()?;
                self.pop_scope();
                self.fn_depth = saved;
            }
            return self.semi();
        }
        self.err("expected `(`")
    }

    /// A property name: identifier/keyword, string, number, private name or `[expr]`.
    fn property_key(&mut self) -> R<()> {
        match self.kind(0) {
            Some(Kind::Ident) | Some(Kind::Str) | Some(Kind::Num) | Some(Kind::Private) => {
                self.bump();
                Ok(())
            }
            Some(Kind::Punct) if self.text(0) == "[" => {
                self.bump();
                self.assign()?;
                self.expect("]")
            }
            _ => self.err("expected a property name"),
        }
    }

    fn decorators(&mut self) -> R<()> {
        while self.eat("@") {
            self.lhs(true)?;
        }
        Ok(())
    }

    fn for_statement(&mut self) -> R<()> {
        self.bump();
        self.eat("await");
        self.expect("(")?;
        self.push_scope(ScopeKind::Block);
        if self.is_punct(0, ";") {
            self.bump();
        } else {
            let is_decl = self.is_word(0, "var") || self.is_word(0, "const") || (self.is_word(0, "let") && (self.is_ident(1) || self.is_punct(1, "[") || self.is_punct(1, "{")));
            if is_decl {
                let var = self.is_word(0, "var");
                self.bump();
                let mut names = Vec::new();
                loop {
                    self.binding(var, &mut names)?;
                    self.type_annotation()?;
                    if self.eat("=") {
                        let saved = std::mem::replace(&mut self.no_in, true);
                        let r = self.assign();
                        self.no_in = saved;
                        r?;
                    }
                    if !self.eat(",") {
                        break;
                    }
                }
            } else {
                let saved = std::mem::replace(&mut self.no_in, true);
                let r = self.expression();
                self.no_in = saved;
                r?;
            }
            if self.eat("of") {
                self.assign()?;
                self.expect(")")?;
                let r = self.sub_statement();
                self.pop_scope();
                return r;
            }
            if self.eat("in") {
                self.expression()?;
                self.expect(")")?;
                let r = self.sub_statement();
                self.pop_scope();
                return r;
            }
            self.expect(";")?;
        }
        if !self.is_punct(0, ";") {
            self.expression()?;
        }
        self.expect(";")?;
        if !self.is_punct(0, ")") {
            self.expression()?;
        }
        self.expect(")")?;
        let r = self.sub_statement();
        self.pop_scope();
        r
    }

    fn try_statement(&mut self) -> R<()> {
        self.bump();
        self.block()?;
        if self.eat("catch") {
            self.push_scope(ScopeKind::Block);
            if self.eat("(") {
                let mut names = Vec::new();
                self.binding(false, &mut names)?;
                self.type_annotation()?;
                self.expect(")")?;
            }
            self.block()?;
            self.pop_scope();
        }
        if self.eat("finally") {
            self.block()?;
        }
        Ok(())
    }

    fn switch_statement(&mut self) -> R<()> {
        self.bump();
        self.paren_expr()?;
        self.expect("{")?;
        self.push_scope(ScopeKind::Block);
        while !self.is_punct(0, "}") {
            if self.eof() {
                return self.err("expected `}`");
            }
            if self.eat("case") {
                self.expression()?;
                self.expect(":")?;
            } else if self.eat("default") {
                self.expect(":")?;
            } else {
                self.statement(false)?;
            }
        }
        self.bump();
        self.pop_scope();
        Ok(())
    }

    // ------------------------------------------------------------ module declarations

    fn import_decl(&mut self) -> R<()> {
        let start = self.pos();
        self.bump();
        // `import "m"`
        if self.kind(0) == Some(Kind::Str) {
            let lit = self.text(0);
            let source = self.add_source(lit);
            self.import_decls.push(ImportDecl { source, side_effect: true });
            self.bump();
            self.import_attributes()?;
            self.semi()?;
            self.m.removals.push((start, self.prev_end()));
            return Ok(());
        }
        if self.ts {
            // `import type ...`: types only
            if self.is_word(0, "type") && (self.is_punct(1, "{") || self.is_punct(1, "*") || (self.is_ident(1) && !self.is_word(1, "from")) || (self.is_word(1, "from") && self.is_word(2, "from"))) {
                while !self.eof() && self.kind(0) != Some(Kind::Str) {
                    self.bump();
                }
                self.bump();
                self.import_attributes()?;
                self.semi()?;
                self.m.removals.push((start, self.prev_end()));
                return Ok(());
            }
            // `import x = require("m")` / `import x = A.B`
            if self.is_ident(0) && self.is_punct(1, "=") {
                let name = self.text(0).to_string();
                self.m.replacements.push((start, self.toks[self.i - 1].end, "var".into()));
                self.declare(&name, true);
                self.bump();
                self.bump();
                if self.is_word(0, "require") && self.is_punct(1, "(") && self.kind(2) == Some(Kind::Str) {
                    let t = self.toks[self.i];
                    self.m.replacements.push((t.start, t.end, "__barm_r".into()));
                    self.m.requires.push(super::bundle::unquote(self.text(2)));
                    self.bump();
                    self.arguments()?;
                } else {
                    self.expression()?;
                }
                return self.semi();
            }
        }
        let mut pending: Vec<(String, ImportName)> = Vec::new();
        if self.is_ident(0) && !self.is_punct(0, "{") {
            // default (but not `import { ...`): note `import from from "x"` is legal
            pending.push((self.text(0).to_string(), ImportName::Default));
            self.bump();
            self.eat(",");
        }
        if self.eat("*") {
            self.expect("as")?;
            pending.push((self.text(0).to_string(), ImportName::Namespace));
            self.bump();
        } else if self.eat("{") {
            while !self.is_punct(0, "}") {
                // `{ type X }`, `{ type X as Y }`: a type
                let type_only = self.ts && self.is_word(0, "type") && !(self.is_punct(1, ",") || self.is_punct(1, "}") || (self.is_word(1, "as") && !self.is_ident(2) && !self.is_word(2, "as")));
                if type_only {
                    self.bump();
                }
                let imported = if self.kind(0) == Some(Kind::Str) { super::bundle::unquote(self.text(0)) } else { self.text(0).to_string() };
                self.bump();
                let local = if self.eat("as") {
                    let l = self.text(0).to_string();
                    self.bump();
                    l
                } else {
                    imported.clone()
                };
                if !type_only {
                    pending.push((local, if imported == "default" { ImportName::Default } else { ImportName::Named(imported) }));
                }
                if !self.is_punct(0, "}") {
                    self.expect(",")?;
                }
            }
            self.bump();
        }
        self.expect("from")?;
        if self.kind(0) != Some(Kind::Str) {
            return self.err("expected a module specifier");
        }
        let source = self.add_source(self.text(0));
        self.bump();
        self.import_attributes()?;
        self.semi()?;
        self.import_decls.push(ImportDecl { source, side_effect: !self.ts });
        for (local, imported) in pending {
            let idx = self.m.bindings.len();
            self.m.bindings.push(ImportBinding { local: local.clone(), imported, source });
            self.import_index.insert(local, idx);
        }
        self.m.removals.push((start, self.prev_end()));
        Ok(())
    }

    /// `with { type: "json" }` / `assert { ... }` after a module specifier.
    fn import_attributes(&mut self) -> R<()> {
        if (self.is_word(0, "with") || (self.is_word(0, "assert") && !self.nl_before(0))) && self.is_punct(1, "{") {
            self.bump();
            self.bump();
            while !self.is_punct(0, "}") && !self.eof() {
                self.bump();
            }
            self.expect("}")?;
        }
        Ok(())
    }

    fn export_decl(&mut self) -> R<()> {
        let start = self.pos();
        self.bump();
        // export default ...
        if self.is_word(0, "default") {
            let kw_end = self.toks[self.i].end;
            self.bump();
            if self.ts && self.is_word(0, "interface") {
                self.ts_declaration()?;
                self.strip(start);
                return Ok(());
            }
            if self.ts && self.is_word(0, "abstract") && self.is_word(1, "class") {
                let t = self.toks[self.i];
                self.m.removals.push((t.start, t.end));
                self.bump();
            }
            let is_async_fn = self.is_word(0, "async") && self.is_word(1, "function") && !self.nl_before(1);
            if self.is_word(0, "function") || is_async_fn {
                let fn_tok = if is_async_fn { 1 } else { 0 };
                let star = self.is_punct(fn_tok + 1, "*");
                let name_k = fn_tok + 1 + star as usize;
                if self.is_ident(name_k) {
                    let name = self.text(name_k).to_string();
                    self.m.removals.push((start, kw_end));
                    if is_async_fn {
                        self.bump();
                    }
                    self.function_decl(is_async_fn)?;
                    self.m.exports.push(("default".into(), ExportTarget::Local(name)));
                } else {
                    // anonymous: give it a name so it stays a hoisted declaration
                    let name_at = self.toks[self.i + name_k - 1].end;
                    self.m.removals.push((start, kw_end));
                    self.m.inserts.push((name_at, " __barm_default".into()));
                    self.declare("__barm_default", false);
                    if is_async_fn {
                        self.bump();
                    }
                    self.bump();
                    self.eat("*");
                    self.function_rest(is_async_fn, star, false)?;
                    self.m.exports.push(("default".into(), ExportTarget::Local("__barm_default".into())));
                }
                return Ok(());
            }
            if self.is_word(0, "class") {
                let class_end = self.toks[self.i].end;
                let named = self.is_ident(1) && !self.is_word(1, "extends");
                self.m.removals.push((start, kw_end));
                if !named {
                    self.m.inserts.push((class_end, " __barm_default".into()));
                }
                let name = self.class(true)?.unwrap_or_else(|| {
                    self.declare("__barm_default", false);
                    "__barm_default".into()
                });
                self.m.exports.push(("default".into(), ExportTarget::Local(name)));
                return Ok(());
            }
            // an expression
            self.m.replacements.push((start, kw_end, "var __barm_default =".into()));
            self.declare("__barm_default", true);
            self.assign()?;
            self.semi()?;
            self.m.exports.push(("default".into(), ExportTarget::Local("__barm_default".into())));
            return Ok(());
        }
        if self.ts {
            // `export default interface X {}` / `export default abstract class`
            // (handled above for classes and functions)
            // `export type { A }`, `export type * from "m"`: types only
            if self.is_word(0, "type") && (self.is_punct(1, "{") || self.is_punct(1, "*")) {
                self.bump();
                if self.is_punct(0, "{") {
                    self.skip_balanced()?;
                } else {
                    while !self.is_word(0, "from") && !self.eof() {
                        self.bump();
                    }
                }
                if self.eat("from") {
                    self.bump();
                    self.import_attributes()?;
                }
                self.semi()?;
                self.strip(start);
                return Ok(());
            }
            // `export as namespace X;`
            if self.is_word(0, "as") && self.is_word(1, "namespace") {
                self.bump();
                self.bump();
                self.bump();
                self.semi()?;
                self.strip(start);
                return Ok(());
            }
            if self.is_punct(0, "=") {
                return self.err("`export =` isn't supported yet");
            }
            let kw_end = self.toks[self.i - 1].end;
            let removals = self.m.removals.len();
            if let Some(names) = self.ts_declaration()? {
                self.m.removals.insert(removals, (start, kw_end));
                for n in names {
                    self.m.exports.push((n.clone(), ExportTarget::Local(n)));
                }
                return Ok(());
            }
        }
        // export * from "m" / export * as ns from "m"
        if self.eat("*") {
            let alias = if self.eat("as") {
                let a = if self.kind(0) == Some(Kind::Str) { super::bundle::unquote(self.text(0)) } else { self.text(0).to_string() };
                self.bump();
                Some(a)
            } else {
                None
            };
            self.expect("from")?;
            let source = self.add_source(self.text(0));
            self.bump();
            self.import_attributes()?;
            self.semi()?;
            match alias {
                Some(a) => self.m.exports.push((a, ExportTarget::Import { source, name: ImportName::Namespace })),
                None => self.m.stars.push(source),
            }
            self.m.removals.push((start, self.prev_end()));
            return Ok(());
        }
        // export { a, b as c } [from "m"]
        if self.eat("{") {
            let mut items = Vec::new();
            while !self.is_punct(0, "}") {
                let type_only = self.ts && self.is_word(0, "type") && !(self.is_punct(1, ",") || self.is_punct(1, "}") || (self.is_word(1, "as") && !self.is_ident(2) && !self.is_word(2, "as")));
                if type_only {
                    self.bump();
                }
                let local = if self.kind(0) == Some(Kind::Str) { super::bundle::unquote(self.text(0)) } else { self.text(0).to_string() };
                self.bump();
                let exported = if self.eat("as") {
                    let e = if self.kind(0) == Some(Kind::Str) { super::bundle::unquote(self.text(0)) } else { self.text(0).to_string() };
                    self.bump();
                    e
                } else {
                    local.clone()
                };
                if !type_only {
                    items.push((local, exported));
                }
                if !self.is_punct(0, "}") {
                    self.expect(",")?;
                }
            }
            self.bump();
            if self.eat("from") {
                let source = self.add_source(self.text(0));
                self.bump();
                self.import_attributes()?;
                self.semi()?;
                for (local, exported) in items {
                    let name = if local == "default" { ImportName::Default } else { ImportName::Named(local) };
                    self.m.exports.push((exported, ExportTarget::Import { source, name }));
                }
            } else {
                self.semi()?;
                for (local, exported) in items {
                    self.m.exports.push((exported, ExportTarget::Local(local)));
                }
            }
            self.m.removals.push((start, self.prev_end()));
            return Ok(());
        }
        // export var/let/const/function/class
        let kw_end = self.toks[self.i - 1].end;
        self.m.removals.push((start, kw_end));
        let names = if self.is_word(0, "var") || self.is_word(0, "let") || self.is_word(0, "const") {
            let var = self.is_word(0, "var");
            self.var_statement(var)?
        } else if self.is_word(0, "function") || (self.is_word(0, "async") && self.is_word(1, "function")) {
            let is_async = self.eat("async");
            let k = 1 + self.is_punct(1, "*") as usize;
            let name = self.text(k).to_string();
            self.function_decl(is_async)?;
            vec![name]
        } else if self.is_word(0, "class") {
            self.class(true)?.into_iter().collect()
        } else {
            return self.err("expected a declaration after `export`");
        };
        for n in names {
            self.m.exports.push((n.clone(), ExportTarget::Local(n)));
        }
        Ok(())
    }

    // ------------------------------------------------------------ expressions

    fn expression(&mut self) -> R<()> {
        self.assign()?;
        while self.eat(",") {
            self.assign()?;
        }
        Ok(())
    }

    /// The closing bracket matching the one at token `k` (by bracket nesting).
    fn matching(&self, k: usize) -> Option<usize> {
        let mut depth = 0i32;
        let mut j = self.i + k;
        while j < self.toks.len() {
            let t = &self.toks[j];
            match t.kind {
                Kind::Punct => match t.text(self.src) {
                    "(" | "[" | "{" => depth += 1,
                    ")" | "]" | "}" => {
                        depth -= 1;
                        if depth == 0 {
                            return Some(j - self.i);
                        }
                    }
                    _ => {}
                },
                Kind::TemplateHead => depth += 1,
                Kind::TemplateTail => {
                    depth -= 1;
                    if depth == 0 {
                        return Some(j - self.i);
                    }
                }
                _ => {}
            }
            j += 1;
        }
        None
    }

    fn arrow_ahead(&mut self) -> bool {
        // x => / async x => / (..) => / async (..) =>
        if self.is_ident(0) && self.is_punct(1, "=>") && !self.nl_before(1) {
            return true;
        }
        if self.is_word(0, "async") && self.is_ident(1) && self.is_punct(2, "=>") && !self.nl_before(1) {
            return true;
        }
        let k = if self.is_word(0, "async") && (self.is_punct(1, "(") || self.is_punct(1, "<")) && !self.nl_before(1) { 1 } else { 0 };
        if self.ts && self.is_punct(k, "<") {
            // `<T>(x: T) => x`
            let save = self.i;
            self.i += k;
            let ok = self.skip_angles(false).is_ok() && self.is_punct(0, "(") && self.arrow_after_params(0);
            self.i = save;
            return ok;
        }
        self.is_punct(k, "(") && self.arrow_after_params(k)
    }

    /// The parenthesized list at token `k` is followed by `=>` (or, in TypeScript, by a return
    /// type and `=>`).
    fn arrow_after_params(&mut self, k: usize) -> bool {
        let Some(close) = self.matching(k) else { return false };
        if self.nl_before(close + 1) {
            return false;
        }
        if self.is_punct(close + 1, "=>") {
            return true;
        }
        if self.ts && self.is_punct(close + 1, ":") {
            let save = self.i;
            self.i += close + 2;
            let ok = self.ty().is_ok() && self.is_punct(0, "=>");
            self.i = save;
            return ok;
        }
        false
    }

    fn arrow(&mut self) -> R<()> {
        let is_async = self.is_word(0, "async") && !self.is_punct(1, "=>");
        if is_async {
            self.bump();
        }
        let saved = (self.in_async, self.in_generator);
        self.in_async = is_async;
        self.in_generator = false;
        self.push_scope(ScopeKind::Function);
        self.type_params()?;
        if self.is_punct(0, "(") {
            self.params()?;
            self.return_type()?;
        } else {
            let name = self.text(0).to_string();
            self.declare(&name, true);
            self.bump();
        }
        self.expect("=>")?;
        if self.is_punct(0, "{") {
            self.function_body()?;
        } else {
            let saved_in = std::mem::replace(&mut self.no_in, false);
            let r = self.assign();
            self.no_in = saved_in;
            r?;
        }
        self.pop_scope();
        (self.in_async, self.in_generator) = saved;
        Ok(())
    }

    fn assign(&mut self) -> R<()> {
        if self.arrow_ahead() {
            return self.arrow();
        }
        if self.in_generator && self.is_word(0, "yield") {
            self.bump();
            if self.eat("*") {
                return self.assign();
            }
            if !self.nl_before(0) && !self.eof() && !matches!(self.text(0), ")" | "]" | "}" | "," | ";" | ":") {
                self.assign()?;
            }
            return Ok(());
        }
        self.conditional()?;
        if self.kind(0) == Some(Kind::Punct)
            && matches!(self.text(0), "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "**=" | "<<=" | ">>=" | ">>>=" | "&=" | "|=" | "^=" | "&&=" | "||=" | "??=")
        {
            self.bump();
            self.assign()?;
        }
        Ok(())
    }

    fn conditional(&mut self) -> R<()> {
        self.binary(0)?;
        if self.eat("?") {
            let saved = std::mem::replace(&mut self.no_in, false);
            let r = self.assign();
            self.no_in = saved;
            r?;
            self.expect(":")?;
            self.assign()?;
        }
        Ok(())
    }

    fn binary_prec(&self) -> Option<u8> {
        let t = self.tok(0)?;
        let s = t.text(self.src);
        let p = match t.kind {
            Kind::Punct => match s {
                "??" => 1,
                "||" => 2,
                "&&" => 3,
                "|" => 4,
                "^" => 5,
                "&" => 6,
                "==" | "!=" | "===" | "!==" => 7,
                "<" | ">" | "<=" | ">=" => 8,
                "<<" | ">>" | ">>>" => 9,
                "+" | "-" => 10,
                "*" | "/" | "%" => 11,
                "**" => 12,
                _ => return None,
            },
            Kind::Ident => match s {
                "instanceof" => 8,
                "in" if !self.no_in => 8,
                _ => return None,
            },
            // `/` read as a regular expression where an operator is expected: division
            Kind::Regex => 11,
            _ => return None,
        };
        Some(p)
    }

    fn binary(&mut self, min: u8) -> R<()> {
        self.unary()?;
        loop {
            if self.ts && (self.is_word(0, "as") || self.is_word(0, "satisfies")) && !self.nl_before(0) {
                let start = self.pos();
                self.bump();
                if !self.eat("const") {
                    self.ty()?;
                }
                self.strip(start);
                continue;
            }
            let Some(prec) = self.binary_prec() else { break };
            if prec < min {
                break;
            }
            if self.kind(0) == Some(Kind::Regex) {
                self.relex(false)?;
            }
            self.bump();
            // `**` is right-associative
            let next = if prec == 12 { prec } else { prec + 1 };
            self.binary(next)?;
        }
        Ok(())
    }

    fn unary(&mut self) -> R<()> {
        if self.kind(0) == Some(Kind::Punct) && matches!(self.text(0), "!" | "~" | "+" | "-" | "++" | "--") {
            self.bump();
            return self.unary();
        }
        if self.kind(0) == Some(Kind::Ident) && matches!(self.text(0), "typeof" | "void" | "delete") {
            self.bump();
            return self.unary();
        }
        if self.ts && !self.jsx && self.is_punct(0, "<") {
            // `<T>x`: a type assertion
            self.type_params()?;
            return self.unary();
        }
        if self.is_word(0, "await") && (self.in_async || self.fn_depth == 0) && !self.is_punct(1, "=>") {
            if self.fn_depth == 0 && !self.in_function_scope() {
                self.m.top_level_await.get_or_insert(self.pos());
            }
            self.bump();
            return self.unary();
        }
        self.lhs(false)?;
        if (self.is_punct(0, "++") || self.is_punct(0, "--")) && !self.nl_before(0) {
            self.bump();
        }
        Ok(())
    }

    /// Inside some function (including arrows), not at module top level.
    fn in_function_scope(&self) -> bool {
        let mut s = self.cur;
        while s != NONE {
            if self.scopes[s as usize].kind == ScopeKind::Function {
                return true;
            }
            s = self.scopes[s as usize].parent;
        }
        false
    }

    /// Calls, member access, `new`, tagged templates. `decorator`: stop before a call's
    /// trailing member access? (decorators allow calls).
    fn lhs(&mut self, _decorator: bool) -> R<()> {
        let callee_ident = self.is_ident(0) && (self.is_punct(1, "(") || matches!(self.kind(1), Some(Kind::Template) | Some(Kind::TemplateHead)));
        self.primary(callee_ident)?;
        loop {
            if self.eof() {
                break;
            }
            match (self.kind(0).unwrap(), self.text(0)) {
                (Kind::Punct, ".") | (Kind::Punct, "?.") => {
                    self.bump();
                    match self.kind(0) {
                        Some(Kind::Ident) | Some(Kind::Private) => self.bump(),
                        Some(Kind::Punct) if self.text(0) == "[" => {
                            self.bump();
                            self.expression()?;
                            self.expect("]")?;
                        }
                        Some(Kind::Punct) if self.text(0) == "(" => self.arguments()?,
                        Some(Kind::Template) | Some(Kind::TemplateHead) => self.template()?,
                        _ => return self.err("expected a property name"),
                    }
                }
                (Kind::Punct, "[") => {
                    self.bump();
                    let saved = std::mem::replace(&mut self.no_in, false);
                    let r = self.expression();
                    self.no_in = saved;
                    r?;
                    self.expect("]")?;
                }
                (Kind::Punct, "(") => self.arguments()?,
                (Kind::Template, _) | (Kind::TemplateHead, _) => self.template()?,
                // TypeScript: `x!` (not null)
                (Kind::Punct, "!") if self.ts && !self.nl_before(0) => {
                    let t = self.toks[self.i];
                    self.m.removals.push((t.start, t.end));
                    self.bump();
                }
                // TypeScript: `f<T>(x)`
                (Kind::Punct, "<") if self.ts => {
                    if !self.try_type_args() {
                        break;
                    }
                }
                _ => break,
            }
        }
        Ok(())
    }

    fn arguments(&mut self) -> R<()> {
        self.expect("(")?;
        let saved = std::mem::replace(&mut self.no_in, false);
        while !self.is_punct(0, ")") {
            if self.eof() {
                self.no_in = saved;
                return self.err("expected `)`");
            }
            self.eat("...");
            self.assign()?;
            if !self.is_punct(0, ")") {
                self.expect(",")?;
            }
        }
        self.no_in = saved;
        self.bump();
        Ok(())
    }

    fn template(&mut self) -> R<()> {
        if self.kind(0) == Some(Kind::Template) {
            self.bump();
            return Ok(());
        }
        // TemplateHead expr (TemplateMiddle expr)* TemplateTail
        self.bump();
        loop {
            let saved = std::mem::replace(&mut self.no_in, false);
            let r = self.expression();
            self.no_in = saved;
            r?;
            match self.kind(0) {
                Some(Kind::TemplateMiddle) => self.bump(),
                Some(Kind::TemplateTail) => {
                    self.bump();
                    return Ok(());
                }
                _ => return self.err("expected the rest of the template literal"),
            }
        }
    }

    fn primary(&mut self, callee: bool) -> R<()> {
        let Some(kind) = self.kind(0) else { return self.err("expected an expression") };
        match kind {
            Kind::JsxOpen => self.jsx_element(),
            Kind::JsxClose | Kind::JsxName | Kind::JsxStr | Kind::JsxText | Kind::JsxEnd | Kind::JsxSelfClose => self.err("unexpected JSX"),
            Kind::Num | Kind::Str | Kind::Regex | Kind::Private => {
                // `#x in obj`
                self.bump();
                Ok(())
            }
            Kind::Template | Kind::TemplateHead => self.template(),
            Kind::TemplateMiddle | Kind::TemplateTail => self.err("unexpected template continuation"),
            Kind::Punct => match self.text(0) {
                "(" => {
                    self.bump();
                    let saved = std::mem::replace(&mut self.no_in, false);
                    let r = self.expression();
                    self.no_in = saved;
                    r?;
                    self.expect(")")
                }
                "[" => {
                    self.bump();
                    let saved = std::mem::replace(&mut self.no_in, false);
                    while !self.is_punct(0, "]") {
                        if self.eof() {
                            return self.err("expected `]`");
                        }
                        if self.eat(",") {
                            continue;
                        }
                        self.eat("...");
                        self.assign()?;
                        if !self.is_punct(0, "]") {
                            self.expect(",")?;
                        }
                    }
                    self.no_in = saved;
                    self.bump();
                    Ok(())
                }
                "{" => self.object(),
                "/" | "/=" => {
                    // division guessed where an expression starts: a regular expression
                    self.relex(true)?;
                    self.bump();
                    Ok(())
                }
                "@" => {
                    self.decorators()?;
                    self.primary(callee)
                }
                _ => self.err("expected an expression"),
            },
            Kind::Ident => {
                let w = self.text(0);
                match w {
                    "function" => {
                        self.bump();
                        let star = self.eat("*");
                        self.function_expr_rest(false, star)
                    }
                    "async" if self.is_word(1, "function") && !self.nl_before(1) => {
                        self.bump();
                        self.bump();
                        let star = self.eat("*");
                        self.function_expr_rest(true, star)
                    }
                    "class" => self.class(false).map(|_| ()),
                    "new" => {
                        self.bump();
                        if self.eat(".") {
                            // new.target
                            self.bump();
                            return Ok(());
                        }
                        // the constructor: a member expression without calls
                        self.new_callee()?;
                        if self.is_punct(0, "(") {
                            self.arguments()?;
                        }
                        Ok(())
                    }
                    "import" => {
                        let start = self.pos();
                        if self.is_punct(1, ".") {
                            // import.meta
                            let end = self.toks[self.i + 2].end;
                            self.m.import_meta.push((start, end));
                            self.bump();
                            self.bump();
                            self.bump();
                            return Ok(());
                        }
                        let end = self.toks[self.i].end;
                        self.m.dynamic_imports.push((start, end));
                        if self.kind(2) == Some(Kind::Str) && (self.is_punct(3, ")") || self.is_punct(3, ",")) {
                            let spec = super::bundle::unquote(self.text(2));
                            self.m.requires.push(spec);
                        }
                        self.bump();
                        self.arguments()
                    }
                    "super" | "this" | "null" | "true" | "false" => {
                        if w == "this" {
                            self.reference(0, Ctx::Plain);
                        }
                        self.bump();
                        Ok(())
                    }
                    "require" if self.is_punct(1, "(") && lex::literal_arg(&self.toks[self.i..], self.src, 1).is_some() => {
                        let spec = lex::literal_arg(&self.toks[self.i..], self.src, 1).unwrap_or_default();
                        self.m.requires.push(spec);
                        self.reference(0, Ctx::Callee);
                        self.bump();
                        Ok(())
                    }
                    _ if RESERVED.contains(&w) => self.err("expected an expression"),
                    _ => {
                        self.reference(0, if callee { Ctx::Callee } else { Ctx::Plain });
                        self.bump();
                        Ok(())
                    }
                }
            }
        }
    }

    fn new_callee(&mut self) -> R<()> {
        if self.is_word(0, "new") {
            return self.primary(false);
        }
        self.primary(false)?;
        loop {
            match self.text(0) {
                "." if self.kind(0) == Some(Kind::Punct) => {
                    self.bump();
                    self.bump();
                }
                "[" if self.kind(0) == Some(Kind::Punct) => {
                    self.bump();
                    self.expression()?;
                    self.expect("]")?;
                }
                _ if matches!(self.kind(0), Some(Kind::Template) | Some(Kind::TemplateHead)) => self.template()?,
                "<" if self.ts && self.kind(0) == Some(Kind::Punct) => {
                    if !self.try_type_args() {
                        return Ok(());
                    }
                }
                _ => return Ok(()),
            }
        }
    }

    fn function_expr_rest(&mut self, is_async: bool, generator: bool) -> R<()> {
        // a named function expression's name is visible only inside it
        self.push_scope(ScopeKind::Block);
        if self.kind(0) == Some(Kind::Ident) && !self.is_punct(0, "(") {
            let name = self.text(0).to_string();
            self.declare(&name, false);
            self.bump();
        }
        let r = self.function_rest(is_async, generator, false);
        self.pop_scope();
        r
    }

    fn object(&mut self) -> R<()> {
        self.expect("{")?;
        let saved = std::mem::replace(&mut self.no_in, false);
        while !self.is_punct(0, "}") {
            if self.eof() {
                return self.err("expected `}`");
            }
            if self.eat("...") {
                self.assign()?;
            } else if self.is_ident(0) && (self.is_punct(1, ",") || self.is_punct(1, "}")) {
                // shorthand `{ x }`
                self.reference(0, Ctx::Shorthand);
                self.bump();
            } else if self.is_ident(0) && self.is_punct(1, "=") {
                // `{ x = 1 }` in a destructuring assignment
                self.reference(0, Ctx::Shorthand);
                self.bump();
                self.bump();
                self.assign()?;
            } else {
                // `key: value`, or a method / accessor
                let simple_key = matches!(self.kind(0), Some(Kind::Ident) | Some(Kind::Str) | Some(Kind::Num)) && self.is_punct(1, ":");
                let computed_value = self.is_punct(0, "[") && self.matching(0).is_some_and(|c| self.is_punct(c + 1, ":"));
                if simple_key || computed_value {
                    self.property_key()?;
                    self.expect(":")?;
                    self.assign()?;
                } else {
                    self.member(false)?;
                }
            }
            if !self.is_punct(0, "}") {
                self.expect(",")?;
            }
        }
        self.no_in = saved;
        self.bump();
        Ok(())
    }
}

// ---------------------------------------------------------------- JSX
//
// Elements compile in place to the automatic runtime (React 17+'s, or tsconfig's
// `jsxImportSource`): `<div a="x" {...p}>hi {name}</div>` becomes
// `rt.jsxs("div", {a: "x", ...p, children: ["hi ", name]})`. Embedded expressions stay where they
// are, so their references are rewritten like any other code.

impl<'a> P<'a> {
    /// The runtime module's variable (`__barm_iN`, see esm.rs).
    fn jsx_rt(&mut self) -> String {
        let src = match self.jsx_source {
            Some(s) => s,
            None => {
                let spec = self.jsx_runtime.clone();
                let s = self.add_source(&super::bundle::js_string(&spec));
                self.import_decls.push(ImportDecl { source: s, side_effect: true });
                self.jsx_source = Some(s);
                s
            }
        };
        format!("__barm_i{src}")
    }

    fn jsx_element(&mut self) -> R<()> {
        let rt = self.jsx_rt();
        let open = self.toks[self.i];
        let mut key: Option<String> = None;
        self.bump();
        // the call's head: patched to `jsxs` once the children are counted
        let head = self.m.replacements.len();
        if self.kind(0) == Some(Kind::JsxEnd) {
            // a fragment
            self.m.replacements.push((open.start, open.end, format!("{rt}.jsx({rt}.Fragment, {{")));
        } else {
            let Some(Kind::JsxName) = self.kind(0) else { return self.err("expected a JSX tag name") };
            let name = self.toks[self.i];
            let text = name.text(self.src);
            let intrinsic = text.starts_with(|c: char| c.is_ascii_lowercase()) || text.contains('-') || text.contains(':');
            if intrinsic {
                self.m.replacements.push((open.start, name.end, format!("{rt}.jsx({}, {{", super::bundle::js_string(text))));
            } else {
                // a component: its name is a reference (`Foo`, or `ns.Foo`'s `ns`)
                self.m.replacements.push((open.start, open.end, format!("{rt}.jsx(")));
                let first = text.split('.').next().unwrap_or(text);
                self.refs.push(RawRef { start: name.start, end: name.start + first.len() as u32, scope: self.cur, ctx: Ctx::Plain });
                self.m.inserts.push((name.end, ", {".into()));
            }
            self.bump();
            key = self.jsx_attributes()?;
        }
        // `key`: the call's third argument, as Babel and TypeScript compile it
        let close_args = match &key {
            Some(k) => format!("}}, {k})"),
            None => "})".to_string(),
        };
        if self.kind(0) == Some(Kind::JsxSelfClose) {
            let t = self.toks[self.i];
            self.m.replacements.push((t.start, t.end, close_args));
            self.bump();
            return Ok(());
        }
        // `>`: the children follow
        let gt = self.toks[self.i];
        let children_at = self.m.replacements.len();
        self.m.replacements.push((gt.start, gt.end, String::new()));
        self.bump();
        let mut count = 0usize;
        loop {
            match self.kind(0) {
                Some(Kind::JsxText) => {
                    let t = self.toks[self.i];
                    let text = jsx_text(t.text(self.src));
                    if text.is_empty() {
                        self.m.removals.push((t.start, t.end));
                    } else {
                        self.m.replacements.push((t.start, t.end, format!("{}, ", super::bundle::js_string(&text))));
                        count += 1;
                    }
                    self.bump();
                }
                Some(Kind::Punct) if self.text(0) == "{" => {
                    let lb = self.toks[self.i];
                    self.bump();
                    if self.is_punct(0, "}") {
                        // `{}` or `{/* a comment */}`: nothing
                        let rb = self.toks[self.i];
                        self.m.removals.push((lb.start, rb.end));
                        self.bump();
                        continue;
                    }
                    self.m.removals.push((lb.start, lb.end));
                    let spread = self.eat("...");
                    let saved = std::mem::replace(&mut self.no_in, false);
                    let r = self.assign();
                    self.no_in = saved;
                    r?;
                    let rb = self.toks.get(self.i).copied();
                    self.expect("}")?;
                    if let Some(rb) = rb {
                        self.m.replacements.push((rb.start, rb.end, ", ".into()));
                    }
                    // (a spread child counts as many)
                    count += if spread { 2 } else { 1 };
                }
                Some(Kind::JsxOpen) => {
                    self.jsx_element()?;
                    self.m.inserts.push((self.prev_end(), ", ".into()));
                    count += 1;
                }
                Some(Kind::JsxClose) => {
                    let start = self.toks[self.i].start;
                    self.bump();
                    if self.kind(0) == Some(Kind::JsxName) {
                        self.bump();
                    }
                    let Some(Kind::JsxEnd) = self.kind(0) else { return self.err("expected `>`") };
                    let end = self.toks[self.i].end;
                    self.bump();
                    let (open_text, close_text) = match count {
                        0 => ("", close_args.clone()),
                        1 => ("children: ", close_args.clone()),
                        _ => ("children: [", format!("]{close_args}")),
                    };
                    self.m.replacements[children_at].2 = open_text.to_string();
                    self.m.replacements.push((start, end, close_text));
                    if count > 1 {
                        // static children: jsxs
                        let h = &mut self.m.replacements[head].2;
                        *h = h.replacen(".jsx(", ".jsxs(", 1);
                    }
                    return Ok(());
                }
                _ => return self.err("expected JSX children or a closing tag"),
            }
        }
    }

    /// Attributes, up to `>` or `/>`: `name="v"`, `name={e}`, `name`, `{...e}`. Returns the
    /// `key`'s code when it can move to the call's third argument.
    fn jsx_attributes(&mut self) -> R<Option<String>> {
        let mut key_out = None;
        loop {
            match self.kind(0) {
                Some(Kind::JsxEnd) | Some(Kind::JsxSelfClose) => return Ok(key_out),
                Some(Kind::JsxName) => {
                    let name = self.toks[self.i];
                    let key = super::bundle::js_string(name.text(self.src));
                    self.bump();
                    if !self.is_punct(0, "=") {
                        self.m.replacements.push((name.start, name.end, format!("{key}: true, ")));
                        continue;
                    }
                    self.bump();
                    let is_key = name.text(self.src) == "key";
                    match self.kind(0) {
                        Some(Kind::JsxStr) => {
                            let v = self.toks[self.i];
                            let raw = v.text(self.src);
                            let value = super::bundle::js_string(&jsx_entities(&raw[1..raw.len() - 1]));
                            if is_key {
                                self.m.removals.push((name.start, v.end));
                                key_out = Some(value);
                            } else {
                                self.m.replacements.push((name.start, v.end, format!("{key}: {value}, ")));
                            }
                            self.bump();
                        }
                        Some(Kind::Punct) if self.text(0) == "{" && is_key => {
                            // moved as text when nothing in it needs rewriting (no imports, JSX
                            // or types); otherwise it stays a prop (React reads it from there too)
                            let lb = self.toks[self.i];
                            let (refs, edits) = (self.refs.len(), self.m.removals.len() + self.m.replacements.len() + self.m.inserts.len());
                            self.bump();
                            let from = self.pos();
                            let saved = std::mem::replace(&mut self.no_in, false);
                            let r = self.assign();
                            self.no_in = saved;
                            r?;
                            let to = self.prev_end();
                            let rb = self.toks.get(self.i).copied();
                            self.expect("}")?;
                            let clean = self.m.removals.len() + self.m.replacements.len() + self.m.inserts.len() == edits
                                && self.refs[refs..].iter().all(|r| !self.import_index.contains_key(&self.src[r.start as usize..r.end as usize]))
                                && self.m.top_this.last().is_none_or(|t| t.0 < from);
                            match (clean, rb) {
                                (true, Some(rb)) => {
                                    self.m.removals.push((name.start, rb.end));
                                    key_out = Some(format!("({})", &self.src[from as usize..to as usize]));
                                }
                                (_, Some(rb)) => {
                                    self.m.replacements.push((name.start, lb.end, format!("{key}: (")));
                                    self.m.replacements.push((rb.start, rb.end, "), ".into()));
                                }
                                _ => {}
                            }
                        }
                        Some(Kind::Punct) if self.text(0) == "{" => {
                            let lb = self.toks[self.i];
                            self.m.replacements.push((name.start, lb.end, format!("{key}: (")));
                            self.bump();
                            let saved = std::mem::replace(&mut self.no_in, false);
                            let r = self.assign();
                            self.no_in = saved;
                            r?;
                            let rb = self.toks.get(self.i).copied();
                            self.expect("}")?;
                            if let Some(rb) = rb {
                                self.m.replacements.push((rb.start, rb.end, "), ".into()));
                            }
                        }
                        Some(Kind::JsxOpen) => {
                            // an element as a value
                            self.m.replacements.push((name.start, self.toks[self.i - 1].end, format!("{key}: ")));
                            self.jsx_element()?;
                            self.m.inserts.push((self.prev_end(), ", ".into()));
                        }
                        _ => return self.err("expected an attribute value"),
                    }
                }
                Some(Kind::Punct) if self.text(0) == "{" => {
                    // `{...props}`
                    let lb = self.toks[self.i];
                    self.m.removals.push((lb.start, lb.end));
                    self.bump();
                    self.expect("...")?;
                    let saved = std::mem::replace(&mut self.no_in, false);
                    let r = self.assign();
                    self.no_in = saved;
                    r?;
                    let rb = self.toks.get(self.i).copied();
                    self.expect("}")?;
                    if let Some(rb) = rb {
                        self.m.replacements.push((rb.start, rb.end, ", ".into()));
                    }
                }
                _ => return self.err("expected a JSX attribute"),
            }
        }
    }
}

/// JSX text as a string: lines trimmed (but spaces between words on a line kept), blank lines
/// dropped, the rest joined with spaces; entities decoded (as Babel and TypeScript do).
fn jsx_text(raw: &str) -> String {
    let lines: Vec<&str> = raw.split('\n').collect();
    let last = lines.len() - 1;
    let mut out = String::new();
    for (i, line) in lines.iter().enumerate() {
        let line = line.trim_end_matches('\r');
        let mut l = line;
        if i > 0 {
            l = l.trim_start_matches([' ', '\t']);
        }
        if i < last {
            l = l.trim_end_matches([' ', '\t']);
        }
        if l.is_empty() {
            continue;
        }
        if !out.is_empty() {
            out.push(' ');
        }
        out.push_str(l);
    }
    jsx_entities(&out)
}

/// HTML character references in JSX text and attribute strings.
fn jsx_entities(s: &str) -> String {
    if !s.contains('&') {
        return s.to_string();
    }
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(i) = rest.find('&') {
        out.push_str(&rest[..i]);
        rest = &rest[i..];
        let end = rest.find(';').filter(|&e| e <= 10);
        let decoded = end.and_then(|e| {
            let name = &rest[1..e];
            let ch = if let Some(h) = name.strip_prefix("#x").or_else(|| name.strip_prefix("#X")) {
                u32::from_str_radix(h, 16).ok().and_then(char::from_u32)
            } else if let Some(d) = name.strip_prefix('#') {
                d.parse().ok().and_then(char::from_u32)
            } else {
                match name {
                    "amp" => Some('&'),
                    "lt" => Some('<'),
                    "gt" => Some('>'),
                    "quot" => Some('"'),
                    "apos" => Some('\''),
                    "nbsp" => Some('\u{a0}'),
                    "copy" => Some('©'),
                    "reg" => Some('®'),
                    "trade" => Some('™'),
                    "hellip" => Some('…'),
                    "mdash" => Some('—'),
                    "ndash" => Some('–'),
                    "lsquo" => Some('‘'),
                    "rsquo" => Some('’'),
                    "ldquo" => Some('“'),
                    "rdquo" => Some('”'),
                    "times" => Some('×'),
                    "middot" => Some('·'),
                    "bull" => Some('•'),
                    "larr" => Some('←'),
                    "rarr" => Some('→'),
                    _ => None,
                }
            };
            ch.map(|c| (c, e))
        });
        match decoded {
            Some((c, e)) => {
                out.push(c);
                rest = &rest[e + 1..];
            }
            None => {
                out.push('&');
                rest = &rest[1..];
            }
        }
    }
    out.push_str(rest);
    out
}

// ---------------------------------------------------------------- TypeScript
//
// Types are skipped (they only move `self.i`) and recorded as removals by the caller, which
// knows where the stripped text starts. Speculative skips (type arguments of calls, arrow
// function return types) restore `self.i` when they don't fit.

impl<'a> P<'a> {
    /// Records the text from `start` to the end of the previous token as removed.
    fn strip(&mut self, start: u32) {
        let end = self.prev_end();
        if end > start {
            self.m.removals.push((start, end));
        }
    }

    /// Strips `?`, `!` and `: Type` after a binding name or pattern.
    fn type_annotation(&mut self) -> R<()> {
        if !self.ts {
            return Ok(());
        }
        let start = self.pos();
        if self.is_punct(0, "?") || self.is_punct(0, "!") {
            self.bump();
        }
        if self.eat(":") {
            self.ty()?;
        }
        self.strip(start);
        Ok(())
    }

    /// Strips `<T, U extends X = Y>` type parameters, if there are any.
    fn type_params(&mut self) -> R<()> {
        if self.ts && self.is_punct(0, "<") {
            let start = self.pos();
            self.skip_angles(true)?;
            self.strip(start);
        }
        Ok(())
    }

    /// Skips the brackets opening at the current token.
    fn skip_balanced(&mut self) -> R<()> {
        match self.matching(0) {
            Some(k) => {
                self.i += k + 1;
                Ok(())
            }
            None => self.err("unbalanced brackets"),
        }
    }

    /// Skips `<...>` (type parameters or arguments). `split`: a closing `>=` / `>>` that also
    /// holds the next token's text is split (`let x: A<B>= y`); speculative skips fail instead.
    fn skip_angles(&mut self, split: bool) -> R<()> {
        let mut depth = 0i32;
        loop {
            if self.eof() {
                return self.err("expected `>`");
            }
            if self.kind(0) == Some(Kind::Punct) {
                let t = self.text(0);
                match t {
                    "<" => depth += 1,
                    "(" | "[" | "{" => {
                        self.skip_balanced()?;
                        continue;
                    }
                    ")" | "]" | "}" | ";" | "&&" | "||" if !split => return self.err("not type arguments"),
                    _ if t.starts_with('>') => {
                        let closes = t.bytes().take_while(|&b| b == b'>').count() as i32;
                        if closes > depth || (closes == depth && t.len() > closes as usize) {
                            if !split {
                                return self.err("not type arguments");
                            }
                            // split off the `>`s that close this list
                            let take = depth.min(closes) as u32;
                            let tok = self.toks[self.i];
                            self.splits.push((tok.start, take));
                            let first = Tok { kind: Kind::Punct, start: tok.start, end: tok.start + take, nl_before: tok.nl_before };
                            let rest = Tok { kind: Kind::Punct, start: tok.start + take, end: tok.end, nl_before: false };
                            self.toks[self.i] = first;
                            self.toks.insert(self.i + 1, rest);
                            self.bump();
                            return Ok(());
                        }
                        depth -= closes;
                    }
                    _ => {}
                }
            }
            self.bump();
            if depth == 0 {
                return Ok(());
            }
        }
    }

    /// `<T>` type arguments before a call or template (`f<T>(x)`), or ending an instantiation
    /// expression: stripped if they parse, otherwise `<` is an operator.
    fn try_type_args(&mut self) -> bool {
        let (save, start) = (self.i, self.pos());
        if self.skip_angles(false).is_ok() {
            let ok = match self.kind(0) {
                None => true,
                Some(Kind::Template) | Some(Kind::TemplateHead) => true,
                Some(Kind::Punct) => matches!(self.text(0), "(" | ")" | "]" | ";" | "," | "}" | "." | "?.") || self.nl_before(0),
                _ => self.nl_before(0),
            };
            if ok {
                self.strip(start);
                return true;
            }
        }
        self.i = save;
        false
    }

    /// Skips a type.
    fn ty(&mut self) -> R<()> {
        self.ty_union()?;
        if self.is_word(0, "extends") && !self.nl_before(0) {
            self.bump();
            self.ty_union()?;
            self.expect("?")?;
            self.ty()?;
            self.expect(":")?;
            self.ty()?;
        }
        Ok(())
    }

    fn ty_union(&mut self) -> R<()> {
        if self.is_punct(0, "|") || self.is_punct(0, "&") {
            self.bump();
        }
        self.ty_operand()?;
        while self.is_punct(0, "|") || self.is_punct(0, "&") {
            self.bump();
            self.ty_operand()?;
        }
        Ok(())
    }

    fn ty_operand(&mut self) -> R<()> {
        while self.kind(0) == Some(Kind::Ident) && matches!(self.text(0), "keyof" | "unique" | "readonly") && !self.is_type_end(1) {
            self.bump();
        }
        if self.is_word(0, "infer") && self.is_ident(1) {
            self.bump();
            self.bump();
            // `infer U extends C` (a constraint, unless it starts a conditional type)
            if self.is_word(0, "extends") {
                let save = self.i;
                self.bump();
                if self.ty_union().is_err() || self.is_punct(0, "?") {
                    self.i = save;
                }
            }
        } else {
            self.ty_primary()?;
        }
        while self.is_punct(0, "[") && !self.nl_before(0) {
            self.skip_balanced()?;
        }
        Ok(())
    }

    /// The token at `k` can't continue a type (so a preceding `keyof` is a type name).
    fn is_type_end(&self, k: usize) -> bool {
        self.kind(k).is_none() || (self.kind(k) == Some(Kind::Punct) && matches!(self.text(k), "," | ")" | "]" | ">" | ";" | "=" | "|" | "&" | "}" | "?" | ":"))
    }

    fn ty_primary(&mut self) -> R<()> {
        let Some(kind) = self.kind(0) else { return self.err("expected a type") };
        match kind {
            Kind::Str | Kind::Num | Kind::Template => self.bump(),
            Kind::TemplateHead => {
                self.bump();
                loop {
                    self.ty()?;
                    match self.kind(0) {
                        Some(Kind::TemplateMiddle) => self.bump(),
                        Some(Kind::TemplateTail) => {
                            self.bump();
                            break;
                        }
                        _ => return self.err("expected the rest of the template literal type"),
                    }
                }
            }
            Kind::Punct => match self.text(0) {
                "-" | "+" => {
                    self.bump();
                    self.bump();
                }
                "(" => {
                    let close = self.matching(0).ok_or_else(|| ParseError { pos: self.pos(), message: "unbalanced `(`".into() })?;
                    if self.is_punct(close + 1, "=>") {
                        self.i += close + 2;
                        self.ty()?;
                    } else {
                        self.bump();
                        self.ty()?;
                        self.expect(")")?;
                    }
                }
                "<" => {
                    // a generic function type
                    self.skip_angles(true)?;
                    self.skip_balanced()?;
                    self.expect("=>")?;
                    self.ty()?;
                }
                "{" | "[" => self.skip_balanced()?,
                _ => return self.err("expected a type"),
            },
            Kind::Ident => match self.text(0) {
                "new" | "abstract" => {
                    self.eat("abstract");
                    self.expect("new")?;
                    if self.is_punct(0, "<") {
                        self.skip_angles(true)?;
                    }
                    self.skip_balanced()?;
                    self.expect("=>")?;
                    self.ty()?;
                }
                "asserts" if self.kind(1) == Some(Kind::Ident) && !self.nl_before(1) && !self.is_word(1, "is") => {
                    self.bump();
                    self.bump();
                    if self.is_word(0, "is") && !self.nl_before(0) {
                        self.bump();
                        self.ty()?;
                    }
                }
                "typeof" | "import" => {
                    if self.eat("typeof") && !self.is_word(0, "import") {
                        self.bump();
                    } else {
                        self.eat("import");
                        self.skip_balanced()?;
                    }
                    while self.is_punct(0, ".") {
                        self.bump();
                        self.bump();
                    }
                    if self.is_punct(0, "<") && !self.nl_before(0) {
                        self.skip_angles(true)?;
                    }
                }
                _ => {
                    self.bump();
                    while self.is_punct(0, ".") {
                        self.bump();
                        self.bump();
                    }
                    if self.is_punct(0, "<") && !self.nl_before(0) {
                        self.skip_angles(true)?;
                    }
                    // a type predicate: `x is T`
                    if self.is_word(0, "is") && !self.nl_before(0) {
                        self.bump();
                        self.ty()?;
                    }
                }
            },
            _ => return self.err("expected a type"),
        }
        Ok(())
    }

    /// A TypeScript-only declaration at the current token (`type`, `interface`, `declare`,
    /// `enum`, `abstract class`, `namespace`). Returns the value names it declares, or None if
    /// the statement is not one.
    fn ts_declaration(&mut self) -> R<Option<Vec<String>>> {
        if !self.ts || self.kind(0) != Some(Kind::Ident) || self.nl_before(1) {
            return Ok(None);
        }
        let start = self.pos();
        match self.text(0) {
            "type" if self.is_ident(1) => {
                self.bump();
                self.bump();
                if self.is_punct(0, "<") {
                    self.skip_angles(true)?;
                }
                self.expect("=")?;
                self.ty()?;
                self.semi()?;
            }
            "interface" if self.is_ident(1) => {
                self.bump();
                self.bump();
                while !self.is_punct(0, "{") {
                    if self.eof() {
                        return self.err("expected `{`");
                    }
                    if self.is_punct(0, "<") {
                        self.skip_angles(true)?;
                    } else {
                        self.bump();
                    }
                }
                self.skip_balanced()?;
            }
            "declare" if self.kind(1) == Some(Kind::Ident) => self.skip_declare()?,
            "abstract" if self.is_word(1, "class") => {
                self.bump();
                self.strip(start);
                return Ok(Some(self.class(true)?.into_iter().collect()));
            }
            "enum" if self.is_ident(1) => return Ok(Some(vec![self.enum_decl(start)?])),
            "const" if self.is_word(1, "enum") => return Ok(Some(vec![self.enum_decl(start)?])),
            "namespace" | "module" if (self.is_ident(1) || self.kind(1) == Some(Kind::Str)) && (self.is_punct(2, "{") || self.is_punct(2, ".")) => {
                return self.err("TypeScript namespaces aren't supported yet");
            }
            _ => return Ok(None),
        }
        self.strip(start);
        Ok(Some(Vec::new()))
    }

    /// Skips `declare ...` (an ambient declaration: no code).
    fn skip_declare(&mut self) -> R<()> {
        self.bump();
        let braced = matches!(self.text(0), "global" | "module" | "namespace" | "class" | "enum" | "abstract") || (self.is_word(0, "const") && self.is_word(1, "enum"));
        let first = self.i;
        loop {
            if self.eof() {
                return Ok(());
            }
            if self.is_punct(0, ";") {
                self.bump();
                return Ok(());
            }
            if self.is_punct(0, "{") && braced {
                return self.skip_balanced();
            }
            if self.i > first && self.nl_before(0) && !braced {
                let prev = self.toks[self.i - 1];
                let ends = matches!(prev.kind, Kind::Ident | Kind::Str | Kind::Num | Kind::Template | Kind::TemplateTail) || matches!(prev.text(self.src), ")" | "]" | "}" | ">");
                let continues = self.kind(0) == Some(Kind::Punct) && matches!(self.text(0), "|" | "&" | "." | "=>" | "?" | ":" | "," | "=" | "<" | ">" | "[" | "(" | "{");
                if ends && !continues && !self.is_word(0, "extends") && !self.is_word(0, "is") {
                    return Ok(());
                }
            }
            if self.kind(0) == Some(Kind::Punct) && matches!(self.text(0), "(" | "[" | "{") {
                self.skip_balanced()?;
            } else if self.is_punct(0, "<") {
                self.skip_angles(true)?;
            } else {
                self.bump();
            }
        }
    }

    /// `[const] enum E { A, B = 2, C = "c" }` becomes
    /// `var E = (function (E) { var A = 0; E[E["A"] = A] = "A"; ... return E; })(E || {});`.
    fn enum_decl(&mut self, start: u32) -> R<String> {
        self.eat("const");
        self.expect("enum")?;
        let name = self.text(0).to_string();
        self.bump();
        self.declare(&name, true);
        self.expect("{")?;
        self.m.replacements.push((start, self.prev_end(), format!("var {name} = (function ({name}) {{")));
        self.push_scope(ScopeKind::Function);
        self.declare(&name, true);
        let mut prev: Option<String> = None;
        while !self.is_punct(0, "}") {
            let kt = self.toks.get(self.i).copied().ok_or_else(|| ParseError { pos: self.pos(), message: "expected `}`".into() })?;
            let key = match kt.kind {
                Kind::Ident => kt.text(self.src).to_string(),
                Kind::Str => super::bundle::unquote(kt.text(self.src)),
                _ => return self.err("expected an enum member name"),
            };
            self.bump();
            let local = (kt.kind == Kind::Ident).then(|| key.clone());
            let k = super::bundle::js_string(&key);
            if self.eat("=") {
                let init_start = self.pos();
                let is_str = matches!(self.kind(0), Some(Kind::Str) | Some(Kind::Template)) && (self.is_punct(1, ",") || self.is_punct(1, "}"));
                if let Some(l) = &local {
                    self.declare(l, true);
                }
                let (head, tail) = match (&local, is_str) {
                    (Some(l), false) => (format!("var {l} = "), format!("; {name}[{name}[{k}] = {l}] = {k};")),
                    (Some(l), true) => (format!("var {l} = "), format!("; {name}[{k}] = {l};")),
                    (None, false) => (format!("{name}[{name}[{k}] = "), format!("] = {k};")),
                    (None, true) => (format!("{name}[{k}] = "), ";".to_string()),
                };
                self.m.replacements.push((kt.start, init_start, head));
                self.assign()?;
                self.m.inserts.push((self.prev_end(), tail));
            } else {
                let value = match &prev {
                    None => "0".to_string(),
                    Some(p) => format!("{p} + 1"),
                };
                let text = match &local {
                    Some(l) => {
                        self.declare(l, true);
                        format!("var {l} = {value}; {name}[{name}[{k}] = {l}] = {k};")
                    }
                    None => format!("{name}[{name}[{k}] = {value}] = {k};"),
                };
                self.m.replacements.push((kt.start, kt.end, text));
            }
            prev = Some(match &local {
                Some(l) => l.clone(),
                None => format!("{name}[{k}]"),
            });
            if self.is_punct(0, ",") {
                let t = self.toks[self.i];
                self.m.removals.push((t.start, t.end));
                self.bump();
            } else if !self.is_punct(0, "}") {
                return self.err("expected `,` or `}`");
            }
        }
        let t = self.toks[self.i];
        self.m.replacements.push((t.start, t.end, format!("return {name}; }})({name} || {{}});")));
        self.bump();
        self.pop_scope();
        Ok(name)
    }

    /// TypeScript class member modifiers (`private`, `readonly`, ...) before a member name:
    /// stripped. Returns true for `declare` and `abstract` members, which have no code.
    fn ts_modifiers(&mut self) -> bool {
        let mut ambient = false;
        while self.ts
            && self.kind(0) == Some(Kind::Ident)
            && matches!(self.text(0), "public" | "private" | "protected" | "readonly" | "abstract" | "override" | "declare")
            && !self.nl_before(1)
            && !(self.kind(1) == Some(Kind::Punct) && matches!(self.text(1), "(" | "=" | ";" | ":" | "?" | "!" | "}" | "," | "<" | ")"))
        {
            ambient |= matches!(self.text(0), "abstract" | "declare");
            let t = self.toks[self.i];
            self.m.removals.push((t.start, t.end));
            self.bump();
        }
        ambient
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn refs(src: &str) -> Vec<String> {
        let m = parse_module(src).unwrap_or_else(|e| panic!("{} at {}", e.message, e.pos));
        m.refs.iter().map(|r| format!("{}{}", &src[r.start as usize..r.end as usize], match r.ctx { Ctx::Callee => "()", Ctx::Shorthand => "{}", Ctx::Plain => "" })).collect()
    }

    #[test]
    fn shadowing() {
        let r = refs("import { a, b } from 'm';\nfunction f(a) { return a + b }\nconst g = () => { let b = 1; return a(b) }\n{ const a = 2; a }\nclass C { m(b) { return b } x = a }");
        assert_eq!(r, vec!["b", "a()", "a"]);
    }

    #[test]
    fn hoisted_var_shadows() {
        let r = refs("import x from 'm'; function f() { x; var x = 1 } x;");
        assert_eq!(r, vec!["x"]);
    }

    #[test]
    fn shorthand_and_members() {
        let r = refs("import { a } from 'm'; const o = { a, b: a.a, [a]: 1, a() {} }; o.a; a`t`;");
        assert_eq!(r, vec!["a{}", "a", "a", "a()"]);
    }

    #[test]
    fn exports() {
        let m = parse_module("import d, { x as y } from 'm';\nexport const a = 1, { b } = o;\nexport default function () {}\nexport { y as z, a as aa };\nexport * from 'n';\nexport * as ns from 'o';\nexport { k } from 'p';").unwrap();
        let ex: Vec<String> = m.exports.iter().map(|(n, t)| format!("{n}={t:?}")).collect();
        assert!(ex.contains(&"a=Local(\"a\")".to_string()));
        assert!(ex.contains(&"b=Local(\"b\")".to_string()));
        assert!(ex.contains(&"default=Local(\"__barm_default\")".to_string()));
        assert!(ex.iter().any(|e| e.starts_with("z=Import")));
        assert_eq!(m.stars, vec![1]);
        assert_eq!(m.sources, vec!["m", "n", "o", "p"]);
    }

    #[test]
    fn regex_after_paren_and_brace() {
        refs("import a from 'm'; if (a) /x/.test(a); function f() {}\n/y/g.exec(a)");
        refs("import a from 'm'; let x = a / 2 / a;");
    }

    #[test]
    fn classes_and_async() {
        refs("import a from 'm'; export class K extends a { static #p = 1; static { a } get v() { return this.#p } async *star() { yield* a; await a } }");
        refs("import a from 'm'; for await (const x of a) {} label: for (let i = 0; i < a; i++) { continue label }");
    }
}
