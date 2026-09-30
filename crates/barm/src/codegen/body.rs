//! Function bodies: statements, expressions, places, conversions and closures.

use super::{c_string, Gen, Val};
use crate::ast::{self, ArrowBody, AssignOp, BinOp, ExprId, ExprKind, StmtId, StmtKind, UnOp};
use crate::check::{Callee, IdentFact, MemberFact};
use crate::hash::{FxMap, FxSet};
use crate::source::Span;
use crate::types::*;
use std::fmt::Write;

/// Maps a case label literal to a union tag.
type LabelTag = Box<dyn Fn(&Gen, crate::intern::Sym) -> Option<usize>>;

pub(crate) enum FnBodyKind {
    Block(StmtId),
    Expr(ExprId),
    /// An implicit constructor: only field initializers.
    Init,
}

#[derive(Clone)]
pub(crate) struct Local {
    /// A C lvalue for the variable's value.
    pub access: String,
    pub ty: TyId,
}

struct Scope {
    /// Release statements for variables declared in this scope, in declaration order.
    releases: Vec<String>,
}

#[derive(Clone)]
enum BreakTarget {
    /// A C loop or C switch: plain `break`.
    Native,
    /// An if-chain switch: `goto label`.
    Goto(String),
}

pub(crate) struct Body {
    pub m: u32,
    pub subst: FxMap<u32, TyId>,
    out: String,
    indent: usize,
    pub locals: FxMap<u32, Local>,
    scopes: Vec<Scope>,
    pub(crate) temps: Vec<Vec<(String, TyId)>>,
    ret: TyId,
    /// (continue label, scope depth at loop body entry)
    loops: Vec<(String, usize)>,
    /// (target, scope depth to unwind to)
    breaks: Vec<(BreakTarget, usize)>,
    /// Locals (by declaration offset) that live in heap cells because a heap closure captures them and they change.
    pub boxed: FxSet<u32>,
    /// Arrow expressions passed directly to built-in methods: their environment can live on the stack.
    pub stack_arrows: FxSet<ExprId>,
    /// Array locals made unique before the enclosing loop and never copied inside it:
    /// element writes skip the copy-on-write check.
    pub unique: FxSet<u32>,
    /// Locals changed anywhere in the function (assigned, mutated in place, passed as `inout`).
    pub mutated: FxSet<u32>,
    /// Read-only (non-`inout`) parameters.
    pub params: FxSet<u32>,
    /// In a derived class's constructor: the class whose field initializers run after `super(...)`.
    pub ctor_class: Option<TyId>,
    /// Enclosing `try` blocks of this function: (label of the handler, scope depth to unwind to).
    pub handlers: Vec<(String, usize)>,
    /// Enclosing `try` blocks that have a `finally`, innermost last: (the `finally` block, scope
    /// depth outside the `try`). `return`, `break` and `continue` run them on the way out.
    pub finallies: Vec<(StmtId, usize)>,
}

impl Body {
    pub(crate) fn bump_indent(&mut self, d: i32) {
        self.indent = (self.indent as i32 + d).max(0) as usize;
    }
}

impl<'c, 'a> Gen<'c, 'a> {
    // ------------------------------------------------------------ emission helpers

    pub(crate) fn b(&mut self) -> &mut Body {
        self.bodies.last_mut().expect("inside a body")
    }

    pub(crate) fn line(&mut self, s: impl AsRef<str>) {
        let b = self.b();
        for _ in 0..b.indent {
            b.out.push_str("    ");
        }
        b.out.push_str(s.as_ref());
        b.out.push('\n');
    }

    pub(crate) fn open(&mut self, s: &str) {
        self.line(s);
        self.b().indent += 1;
    }

    pub(crate) fn close(&mut self, s: &str) {
        self.b().indent -= 1;
        self.line(s);
    }

    pub(crate) fn cur_m(&self) -> u32 {
        self.bodies.last().unwrap().m
    }

    /// The (instantiated) type the checker recorded for an expression.
    pub(crate) fn ty(&mut self, e: ExprId) -> TyId {
        let m = self.cur_m();
        let t = self.facts(m).expr_ty[e as usize];
        let subst = self.bodies.last().unwrap().subst.clone();
        self.c.types.subst(t, &subst)
    }

    pub(crate) fn inst(&mut self, t: TyId) -> TyId {
        let subst = self.bodies.last().unwrap().subst.clone();
        self.c.types.subst(t, &subst)
    }

    pub(crate) fn expr_span(&self, e: ExprId) -> Span {
        self.ast(self.cur_m()).expr(e).span
    }

    /// Declares a temporary holding `code`. Owned temporaries of heap types are released at
    /// the end of the enclosing statement unless consumed.
    pub(crate) fn tmp(&mut self, ty: TyId, code: &str, owned: bool) -> Val {
        let name = self.fresh("t");
        let ct = self.ctype(ty);
        self.line(format!("{ct} {name} = {code};"));
        let owned = owned && self.is_rc(ty);
        if owned {
            self.b().temps.last_mut().unwrap().push((name.clone(), ty));
        }
        Val { code: name, ty, owned }
    }

    /// Returns code for an owned copy of `v` (retaining borrowed heap values).
    pub(crate) fn consume(&mut self, v: Val) -> String {
        if !self.is_rc(v.ty) {
            return v.code;
        }
        if v.owned {
            let b = self.b();
            for scope in b.temps.iter_mut().rev() {
                if let Some(i) = scope.iter().position(|(n, _)| *n == v.code) {
                    scope.remove(i);
                    break;
                }
            }
            return v.code;
        }
        let name = self.fresh("t");
        let ct = self.ctype(v.ty);
        self.line(format!("{ct} {name} = {};", v.code));
        let r = self.retain_code(v.ty, &name);
        self.line(format!("{r};"));
        name
    }

    /// Makes an owned value (for results that must outlive the statement's temporaries).
    pub(crate) fn own(&mut self, v: Val) -> Val {
        if v.owned || !self.is_rc(v.ty) {
            return v;
        }
        let ty = v.ty;
        let code = self.consume(v);
        self.b().temps.last_mut().unwrap().push((code.clone(), ty));
        Val { code, ty, owned: true }
    }

    fn push_temps(&mut self) {
        self.b().temps.push(Vec::new());
    }

    pub(crate) fn push_temps_pub(&mut self) {
        self.push_temps();
    }

    pub(crate) fn pop_temps_pub(&mut self) {
        self.pop_temps();
    }

    /// Starts emitting a helper function body outside any source function (thunks).
    pub(crate) fn begin_scratch(&mut self, m: u32) {
        self.bodies.push(Body {
            m,
            subst: FxMap::default(),
            out: String::new(),
            indent: 1,
            locals: FxMap::default(),
            scopes: vec![Scope { releases: Vec::new() }],
            temps: vec![Vec::new()],
            ret: VOID,
            loops: Vec::new(),
            breaks: Vec::new(),
            boxed: FxSet::default(),
            stack_arrows: FxSet::default(),
            unique: FxSet::default(),
            mutated: FxSet::default(),
            params: FxSet::default(),
            ctor_class: None,
            handlers: Vec::new(),
            finallies: Vec::new(),
        });
    }

    /// Returns `v` (as `ret`, owned) from a scratch body, releasing its temporaries first.
    pub(crate) fn scratch_return(&mut self, v: Val, ret: TyId) {
        if ret == VOID {
            self.release_all_temps();
            self.line("return;");
            return;
        }
        let v = self.coerce(v, ret);
        let code = self.consume(v);
        let ct = self.ctype(ret);
        self.line(format!("{ct} r_ = {code};"));
        self.release_all_temps();
        self.line("return r_;");
    }

    pub(crate) fn end_scratch(&mut self) -> String {
        self.bodies.pop().unwrap().out
    }

    /// After a call that can throw: on an error, clean up and jump to the handler (or return).
    pub(crate) fn error_check(&mut self) {
        self.open("if (__builtin_expect(bmg_err != NULL, 0)) {");
        self.error_path();
        self.close("}");
    }

    /// Leaves the current point with `bmg_err` set: releases temporaries and the scopes being
    /// left, then jumps to the enclosing `try` handler, or returns (the caller checks `bmg_err`).
    fn error_path(&mut self) {
        self.release_all_temps();
        match self.b().handlers.last().cloned() {
            Some((label, depth)) => {
                self.unwind_to(depth);
                self.line(format!("goto {label};"));
            }
            None => {
                self.unwind_to(0);
                let ret = self.b().ret;
                if ret == VOID {
                    self.line("return;");
                } else {
                    let d = self.default_value(ret);
                    self.line(format!("return {d};"));
                }
            }
        }
    }

    /// The base class type of a class type, if any.
    pub(crate) fn class_base_of(&mut self, cls: TyId) -> Option<TyId> {
        let (c, args) = self.c.class_of(cls)?;
        let info = &self.c.classes[c as usize];
        let base = info.base?;
        let map: FxMap<u32, TyId> = info.params.iter().copied().zip(args).collect();
        Some(self.c.types.subst(base, &map))
    }

    /// A local, parameter or field path of one (not a temporary).
    pub(crate) fn is_place_expr(&self, e: ExprId) -> bool {
        let m = self.cur_m();
        match &self.ast(m).expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Member { obj: x, .. } | ExprKind::Index { obj: x, .. } => self.is_place_expr(*x),
            ExprKind::Ident(_) | ExprKind::This => true,
            _ => false,
        }
    }

    fn pop_temps(&mut self) {
        let temps = self.b().temps.pop().unwrap();
        for (name, ty) in temps.into_iter().rev() {
            let r = self.release_code(ty, &name);
            self.line(format!("{r};"));
        }
    }

    fn push_scope(&mut self) {
        self.b().scopes.push(Scope { releases: Vec::new() });
    }

    fn pop_scope(&mut self) {
        let s = self.b().scopes.pop().unwrap();
        for r in s.releases.into_iter().rev() {
            self.line(r);
        }
    }

    /// Emits releases for every scope deeper than `depth` (without closing them).
    fn unwind_to(&mut self, depth: usize) {
        let mut lines = Vec::new();
        for scope in self.b().scopes[depth..].iter().rev() {
            for r in scope.releases.iter().rev() {
                lines.push(r.clone());
            }
        }
        for l in lines {
            self.line(l);
        }
    }

    /// Releases the scopes `to..from` (innermost first), for leaving them in steps.
    fn unwind_range(&mut self, from: usize, to: usize) {
        let mut lines = Vec::new();
        for scope in self.b().scopes[to..from].iter().rev() {
            for r in scope.releases.iter().rev() {
                lines.push(r.clone());
            }
        }
        for l in lines {
            self.line(l);
        }
    }

    /// Leaves to scope depth `depth` (0 for `return`), running the `finally` blocks being left,
    /// innermost first: each runs after the scopes inside its `try` are released, with only the
    /// `finally`s and error handlers outside it active.
    fn unwind_for_exit(&mut self, depth: usize) {
        let fins: Vec<(StmtId, usize)> = self.b().finallies.iter().rev().filter(|(_, d)| *d >= depth).copied().collect();
        let mut prev = self.b().scopes.len();
        for (f, d) in fins {
            self.unwind_range(prev, d);
            prev = d;
            let saved_fins = self.b().finallies.clone();
            let saved_handlers = self.b().handlers.clone();
            self.b().finallies.retain(|(_, dd)| *dd < d);
            self.b().handlers.retain(|(_, dd)| *dd < d);
            self.scoped_block(f);
            self.b().finallies = saved_fins;
            self.b().handlers = saved_handlers;
        }
        self.unwind_range(prev, depth);
    }

    fn release_all_temps(&mut self) {
        let mut lines = Vec::new();
        let all: Vec<(String, TyId)> = self.b().temps.iter().flat_map(|s| s.iter().cloned()).collect();
        for (name, ty) in all.into_iter().rev() {
            lines.push(format!("{};", self.release_code(ty, &name)));
        }
        for l in lines {
            self.line(l);
        }
    }

    // ------------------------------------------------------------ bodies

    /// Emits a function body; returns the C statements (without braces).
    /// `this`: (declaration key, class type) for methods and constructors; `ctor_class`: the class
    /// whose field initializers this constructor runs.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn function_body(
        &mut self,
        m: u32,
        subst: FxMap<u32, TyId>,
        params: &[(u32, TyId, bool)],
        ret: TyId,
        kind: FnBodyKind,
        entry_unique: Option<Vec<u32>>,
        this: Option<(u32, TyId)>,
        ctor_class: Option<TyId>,
    ) -> String {
        let (boxed, stack_arrows, mutated) = self.analyze_closures(m, &kind);
        let mut body = Body {
            m,
            subst,
            out: String::new(),
            indent: 1,
            locals: FxMap::default(),
            scopes: vec![Scope { releases: Vec::new() }],
            temps: vec![Vec::new()],
            ret,
            loops: Vec::new(),
            breaks: Vec::new(),
            boxed,
            stack_arrows,
            unique: FxSet::default(),
            mutated,
            params: params.iter().filter(|p| !p.2).map(|p| p.0).collect(),
            ctor_class,
            handlers: Vec::new(),
            finallies: Vec::new(),
        };
        for (i, &(key, ty, inout)) in params.iter().enumerate() {
            let access = if inout { format!("(*p{i})") } else { format!("p{i}") };
            body.locals.insert(key, Local { access, ty });
        }
        if let Some((key, ty)) = this {
            body.locals.insert(key, Local { access: "self_".into(), ty });
            body.params.insert(key);
        }
        body.unique.extend(entry_unique.unwrap_or_default());
        self.bodies.push(body);
        // Constructors of classes without a base class run their field initializers first.
        if let Some(cls) = ctor_class
            && (matches!(kind, FnBodyKind::Init) || self.class_base_of(cls).is_none())
        {
            self.b().ctor_class = None;
            self.emit_field_inits(cls);
        }
        // Parameters captured by heap closures and reassigned move into cells.
        for (i, &(key, ty, inout)) in params.iter().enumerate() {
            if !inout && self.b().boxed.contains(&key) {
                let cell = self.cell_type(ty);
                let r = self.retain_code(ty, &format!("p{i}"));
                self.line(format!("{cell} *c{i} = bmg_alloc_small(sizeof({cell})); c{i}->rc = 1; c{i}->v = p{i}; {r};"));
                let rel = self.cell_release(ty, &format!("c{i}"));
                self.b().scopes[0].releases.push(rel);
                self.b().locals.insert(key, Local { access: format!("c{i}->v"), ty });
            }
        }
        match kind {
            FnBodyKind::Block(s) => {
                self.stmt_list_of(s);
                self.unwind_to(0);
                if ret != VOID && !self.always_returns_stmt(m, s) {
                    // Only reachable when the return type admits `undefined`.
                    let v = self.coerce(Val::plain("0", UNDEFINED), ret);
                    let code = self.consume(v);
                    self.line(format!("return {code};"));
                }
            }
            FnBodyKind::Expr(e) => {
                let v = self.expr(e);
                if ret == VOID {
                    self.pop_temps();
                    self.b().temps.push(Vec::new());
                    self.unwind_to(0);
                } else {
                    let v = self.coerce(v, ret);
                    let code = self.consume(v);
                    let ct = self.ctype(ret);
                    self.line(format!("{ct} r_ = {code};"));
                    self.release_all_temps();
                    self.unwind_to(0);
                    self.line("return r_;");
                }
            }
            FnBodyKind::Init => self.unwind_to(0),
        }
        let b = self.bodies.pop().unwrap();
        b.out
    }

    fn always_returns_stmt(&self, m: u32, s: StmtId) -> bool {
        // Mirrors the checker's analysis (switch exhaustiveness is recorded there).
        let ast = self.ast(m);
        match &ast.stmt(s).kind {
            StmtKind::Return(_) => true,
            StmtKind::Block(ss) => ss.iter().any(|&x| self.always_returns_stmt(m, x)),
            StmtKind::If(_, t, Some(e)) => self.always_returns_stmt(m, *t) && self.always_returns_stmt(m, *e),
            StmtKind::Switch(_, cases) => {
                cases.iter().any(|c| c.test.is_none()) && cases.iter().all(|c| c.body.is_empty() || c.body.iter().any(|&x| self.always_returns_stmt(m, x)))
                    || (self.c.exhaustive_switch(m, s) && cases.iter().all(|c| c.body.is_empty() || c.body.iter().any(|&x| self.always_returns_stmt(m, x))))
            }
            StmtKind::While(c, _) | StmtKind::For { cond: Some(c), .. } => matches!(ast.expr(*c).kind, ExprKind::Bool(true)),
            StmtKind::For { cond: None, .. } => true,
            StmtKind::DoWhile(body, _) => self.always_returns_stmt(m, *body),
            StmtKind::Throw(_) => true,
            StmtKind::Try { body, catch, finally } => {
                finally.map(|f| self.always_returns_stmt(m, f)).unwrap_or(false)
                    || (self.always_returns_stmt(m, *body) && catch.as_ref().map(|c| self.always_returns_stmt(m, c.body)).unwrap_or(true))
            }
            _ => false,
        }
    }

    /// A block's statements without opening a C block (the function body scope).
    fn stmt_list_of(&mut self, s: StmtId) {
        let m = self.cur_m();
        if let StmtKind::Block(ss) = &self.ast(m).stmt(s).kind {
            for &x in ss.clone().iter() {
                self.stmt(x);
            }
        } else {
            self.stmt(s);
        }
    }

    // ------------------------------------------------------------ closures analysis

    /// Which locals must live in heap cells, and which arrows can keep their environment on the stack.
    fn analyze_closures(&mut self, m: u32, kind: &FnBodyKind) -> (FxSet<u32>, FxSet<ExprId>, FxSet<u32>) {
        let ast = self.ast(m);
        let mut exprs = Vec::new();
        match kind {
            FnBodyKind::Block(s) => collect_exprs_stmt(ast, *s, &mut exprs),
            FnBodyKind::Expr(e) => collect_exprs_expr(ast, *e, &mut exprs),
            FnBodyKind::Init => {}
        }
        let facts = self.facts(m);
        let mut stack_arrows = FxSet::default();
        let mut mutated: FxSet<u32> = FxSet::default();
        let mut heap_captures: FxSet<u32> = FxSet::default();
        let root = |e: ExprId| -> Option<u32> {
            let mut e = e;
            loop {
                match &ast.expr(e).kind {
                    ExprKind::Paren(x) | ExprKind::NonNull(x) => e = *x,
                    ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } => e = *obj,
                    ExprKind::Ident(_) => {
                        return match facts.idents.get(&e) {
                            Some(IdentFact::Local(k)) => Some(*k),
                            _ => None,
                        };
                    }
                    _ => return None,
                }
            }
        };
        for &e in &exprs {
            match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                    if let Some(k) = root(*t) {
                        mutated.insert(k);
                    }
                }
                ExprKind::Call { callee, args, .. } => {
                    for a in args {
                        if a.by_ref.is_some()
                            && let Some(k) = root(a.expr)
                        {
                            mutated.insert(k);
                        }
                    }
                    if let Some(fact) = facts.calls.get(&e)
                        && let ExprKind::Member { obj, name, .. } = &ast.expr(*callee).kind
                            && let Callee::Method(_) = fact.callee
                        {
                            if is_mutating_method(self.c.interner.get(*name))
                                && let Some(k) = root(*obj)
                            {
                                mutated.insert(k);
                            }
                            for a in args {
                                if matches!(ast.expr(a.expr).kind, ExprKind::Arrow(_)) {
                                    stack_arrows.insert(a.expr);
                                }
                            }
                        }
                }
                _ => {}
            }
        }
        for &e in &exprs {
            if matches!(ast.expr(e).kind, ExprKind::Arrow(_)) && !stack_arrows.contains(&e) {
                for k in arrow_captures(ast, facts, e) {
                    heap_captures.insert(k);
                }
            }
        }
        let boxed = heap_captures.intersection(&mutated).copied().collect();
        (boxed, stack_arrows, mutated)
    }

    /// For each local used in `all`: does every use keep an array uniquely owned? Allowed uses:
    /// `x[i]` (read or write), `x.length`, receiving a built-in method that doesn't hand out the
    /// buffer, and `&x` passed to an `inout` parameter that is itself leak-free. Returns the
    /// verdicts and whether each local is passed as `inout`.
    pub(crate) fn uniqueness_uses(&mut self, m: u32, all: &[ExprId]) -> (FxMap<u32, bool>, FxSet<u32>) {
        let ast = self.ast(m);
        let facts = self.facts(m);
        let mut parent: FxMap<ExprId, ExprId> = FxMap::default();
        for &e in all {
            let mut kids = Vec::new();
            match &ast.expr(e).kind {
                ExprKind::Member { obj, .. } => kids.push(*obj),
                ExprKind::Index { obj, index, .. } => {
                    kids.push(*obj);
                    kids.push(*index);
                }
                ExprKind::Call { callee, args, .. } => {
                    kids.push(*callee);
                    kids.extend(args.iter().map(|a| a.expr));
                }
                _ => {}
            }
            for k in kids {
                parent.insert(k, e);
            }
        }
        let mut ok: FxMap<u32, bool> = FxMap::default();
        let mut pending: Vec<(u32, u32, u32, usize)> = Vec::new();
        let mut passed: FxSet<u32> = FxSet::default();
        for &e in all {
            let ExprKind::Ident(_) = ast.expr(e).kind else { continue };
            let Some(IdentFact::Local(k)) = facts.idents.get(&e) else { continue };
            let fine = match parent.get(&e).map(|&p| (p, &ast.expr(p).kind)) {
                Some((_, ExprKind::Index { obj, .. })) if *obj == e => true,
                Some((p, ExprKind::Member { obj, name, .. })) if *obj == e => {
                    let n = self.c.interner.get(*name);
                    let method_call = parent.get(&p).map(|&c| matches!(&ast.expr(c).kind, ExprKind::Call { callee, .. } if *callee == p) && matches!(facts.calls.get(&c).map(|f| &f.callee), Some(Callee::Method(_)))).unwrap_or(false);
                    n == "length" || (method_call && !matches!(n, "slice" | "concat" | "reverse" | "sort"))
                }
                Some((c, ExprKind::Call { args, .. })) => {
                    let idx = args.iter().position(|a| a.expr == e && a.by_ref.is_some());
                    match (idx, facts.calls.get(&c).map(|f| &f.callee)) {
                        (Some(i), Some(Callee::Fn(fm, fi))) => {
                            pending.push((*k, *fm, *fi, i));
                            passed.insert(*k);
                            true
                        }
                        _ => false,
                    }
                }
                _ => false,
            };
            let entry = ok.entry(*k).or_insert(true);
            *entry &= fine;
        }
        for &e in all {
            if matches!(ast.expr(e).kind, ExprKind::Arrow(_)) {
                for k in arrow_captures(ast, facts, e) {
                    ok.insert(k, false);
                }
            }
        }
        for (k, fm, fi, i) in pending {
            if !self.inout_leak_free(fm, fi, i) {
                ok.insert(k, false);
            }
        }
        (ok, passed)
    }

    /// Does function (m, item) keep its `inout` parameter `idx` uniquely owned (never copying,
    /// storing or capturing it)? Such parameters are made unique by callers, and the callee
    /// writes elements without copy-on-write checks.
    pub(crate) fn inout_leak_free(&mut self, m: u32, item: u32, idx: usize) -> bool {
        if let Some(&v) = self.leak_free.get(&(m, item, idx)) {
            return v;
        }
        // Recursion: assume it leaks until proven otherwise.
        self.leak_free.insert((m, item, idx), false);
        let ast = self.ast(m);
        let crate::ast::ItemKind::Function(f) = &ast.items[item as usize].kind else { return false };
        let Some(p) = f.params.get(idx) else { return false };
        if !p.inout {
            return false;
        }
        let key = p.span.start;
        let mut all = Vec::new();
        collect_exprs_stmt(ast, f.body, &mut all);
        let (ok, _) = self.uniqueness_uses(m, &all);
        let v = ok.get(&key).copied().unwrap_or(true);
        self.leak_free.insert((m, item, idx), v);
        v
    }

    /// Makes array locals unique before a loop when every use inside keeps them unique, so
    /// element writes (and leak-free `inout` calls) in the loop skip copy-on-write checks.
    fn hoist_unique(&mut self, stmts: &[StmtId], exprs: &[ExprId]) -> Vec<u32> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let mut all = Vec::new();
        for &s in stmts {
            collect_exprs_stmt(ast, s, &mut all);
        }
        for &e in exprs {
            collect_exprs_expr(ast, e, &mut all);
        }
        let facts = self.facts(m);
        let mut written: FxSet<u32> = FxSet::default();
        for &e in &all {
            if let ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } = &ast.expr(e).kind {
                let mut cur = *t;
                loop {
                    match &ast.expr(cur).kind {
                        ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Member { obj: x, .. } => cur = *x,
                        ExprKind::Index { obj, .. } => {
                            if let (ExprKind::Ident(_), Some(IdentFact::Local(k))) = (&ast.expr(*obj).kind, facts.idents.get(obj)) {
                                written.insert(*k);
                            }
                            cur = *obj;
                        }
                        _ => break,
                    }
                }
            }
        }
        let (ok, passed) = self.uniqueness_uses(m, &all);
        let mut added = Vec::new();
        let candidates: Vec<u32> = ok.iter().filter(|(k, v)| **v && (written.contains(k) || passed.contains(k))).map(|(k, _)| *k).collect();
        for k in candidates {
            if self.b().unique.contains(&k) {
                continue;
            }
            let Some(local) = self.b().locals.get(&k).cloned() else { continue };
            let Ty::Array(et) = self.tget(local.ty) else { continue };
            let d = self.desc(et);
            self.line(format!("bmg_make_unique(&{}, {d});", local.access));
            self.b().unique.insert(k);
            added.push(k);
        }
        added
    }

    fn unhoist(&mut self, keys: Vec<u32>) {
        for k in keys {
            self.b().unique.remove(&k);
        }
    }

    /// A place path (`p`, `p.a.b`) whose root is a read-only parameter, `this` or a local that
    /// never changes, and whose steps through class instances are `readonly` fields.
    fn stable_path(&mut self, e: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        let mut cur = e;
        loop {
            match &ast.expr(cur).kind {
                ExprKind::Paren(x) => cur = *x,
                ExprKind::This => return true,
                ExprKind::Member { obj, name, optional: false, .. } => {
                    let ot = self.ty(*obj);
                    let ot = self.c.types.without_undefined(ot);
                    if self.c.class_of(ot).is_some() {
                        match self.c.class_member(ot, *name) {
                            Some(crate::check::class::ClassMemberRef::Field(f)) if f.readonly && !f.weak => {}
                            _ => return false,
                        }
                    }
                    cur = *obj
                }
                ExprKind::Ident(_) => {
                    return match self.ident_fact(m, cur) {
                        Some(IdentFact::Local(k)) => {
                            let b = self.bodies.last().unwrap();
                            (b.params.contains(&k) || b.locals.contains_key(&k)) && !b.mutated.contains(&k) && !b.boxed.contains(&k)
                        }
                        _ => false,
                    };
                }
                _ => return false,
            }
        }
    }

    pub(crate) fn cell_type(&mut self, ty: TyId) -> String {
        let ct = self.ctype(ty);
        let name = format!("C{}", ty.0);
        if self.helpers_cell_done(ty) {
            let _ = writeln!(self.typedefs, "typedef struct {name} {{ int64_t rc; {ct} v; }} {name};");
        }
        name
    }

    fn helpers_cell_done(&mut self, ty: TyId) -> bool {
        self.helpers_done.insert((ty, 100))
    }

    fn cell_release(&mut self, ty: TyId, cell: &str) -> String {
        let inner = if self.is_rc(ty) { format!("{};", self.release_code(ty, &format!("{cell}->v"))) } else { String::new() };
        format!("if (--{cell}->rc == 0) {{ {inner} bmg_free_small({cell}, sizeof(*{cell})); }}")
    }

    // ------------------------------------------------------------ statements

    pub(crate) fn stmt(&mut self, s: StmtId) {
        let m = self.cur_m();
        let ast = self.ast(m);
        let node = ast.stmt(s);
        match &node.kind {
            StmtKind::Empty | StmtKind::Error => {}
            StmtKind::InitGlobal(ii) => {
                // `g = init_g();` here, in source order (a script's module variable).
                let g = self.const_ref(m, *ii);
                self.line(format!("{} = init_{}();", g.code, g.code));
                if self.c.const_throws.contains(&(m, *ii)) {
                    self.error_check();
                }
            }
            StmtKind::Expr(e) => {
                self.push_temps();
                let v = self.expr(*e);
                // Keep side effects (and traps) of plain expression values.
                if !v.owned && !is_trivial(&v.code) {
                    self.line(format!("(void)({});", v.code));
                }
                self.pop_temps();
            }
            StmtKind::Let { mutable, name, name_span, init, .. } => {
                let ty = self.facts(m).bindings.get(&s).copied().unwrap_or(ERROR);
                let ty = self.inst(ty);
                let uniq = self.fresh("");
                let cname = format!("v_{}_{}", self.sym(*name), uniq);
                let ct = self.ctype(ty);
                let key = name_span.start;
                self.push_temps();
                let value = init.map(|i| {
                    let v = self.expr(i);
                    self.coerce(v, ty)
                });
                // `const x = <field path of a read-only parameter or unchanging local>` that is never
                // changed itself: x is a borrowed view — no retain now, no release at scope end.
                if let (false, Some(i), Some(v)) = (*mutable, init, &value)
                    && !v.owned
                    && !self.b().mutated.contains(&key)
                    && !self.b().boxed.contains(&key)
                    && self.stable_path(*i)
                {
                    self.line(format!("{ct} {cname} = {};", v.code));
                    self.pop_temps();
                    self.b().locals.insert(key, Local { access: cname, ty });
                    return;
                }
                let code = match value {
                    Some(v) => self.consume(v),
                    None => self.default_value(ty),
                };
                if self.b().boxed.contains(&key) {
                    let cell = self.cell_type(ty);
                    self.line(format!("{cell} *{cname} = bmg_alloc_small(sizeof({cell})); {cname}->rc = 1; {cname}->v = {code};"));
                    self.pop_temps();
                    let rel = self.cell_release(ty, &cname);
                    self.b().scopes.last_mut().unwrap().releases.push(rel);
                    self.b().locals.insert(key, Local { access: format!("{cname}->v"), ty });
                } else {
                    self.line(format!("{ct} {cname} = {code};"));
                    self.pop_temps();
                    if self.is_rc(ty) {
                        let rel = format!("{};", self.release_code(ty, &cname));
                        self.b().scopes.last_mut().unwrap().releases.push(rel);
                    }
                    self.b().locals.insert(key, Local { access: cname, ty });
                }
            }
            StmtKind::Block(ss) => {
                self.open("{");
                self.push_scope();
                for &x in ss {
                    self.stmt(x);
                }
                self.pop_scope();
                self.close("}");
            }
            StmtKind::If(c, t, e) => {
                self.push_temps();
                let cond = self.cond(*c);
                let cv = self.fresh("c");
                self.line(format!("bool {cv} = {cond};"));
                self.pop_temps();
                self.open(&format!("if ({cv}) {{"));
                self.scoped_stmt(*t);
                if let Some(e) = e {
                    self.close("} else {");
                    self.b().indent += 1;
                    self.scoped_stmt(*e);
                }
                self.close("}");
            }
            StmtKind::While(c, body) => {
                let hoisted = self.hoist_unique(&[*body], &[*c]);
                let cont = self.fresh("cont");
                self.open("for (;;) {");
                self.cond_break(*c);
                self.loop_body(*body, &cont);
                self.line(format!("{cont}:;"));
                self.close("}");
                self.unhoist(hoisted);
            }
            StmtKind::DoWhile(body, c) => {
                let hoisted = self.hoist_unique(&[*body], &[*c]);
                let cont = self.fresh("cont");
                self.open("for (;;) {");
                self.loop_body(*body, &cont);
                self.line(format!("{cont}:;"));
                self.cond_break(*c);
                self.close("}");
                self.unhoist(hoisted);
            }
            StmtKind::For { init, cond, step, body } => {
                let cont = self.fresh("cont");
                self.open("{");
                self.push_scope();
                if let Some(i) = init {
                    self.stmt(*i);
                }
                let loop_exprs: Vec<ExprId> = [cond, step].into_iter().flatten().copied().collect();
                let hoisted = self.hoist_unique(&[*body], &loop_exprs);
                self.open("for (;;) {");
                if let Some(c) = cond {
                    self.cond_break(*c);
                }
                self.loop_body(*body, &cont);
                self.line(format!("{cont}:;"));
                if let Some(st) = step {
                    self.push_temps();
                    let _ = self.expr(*st);
                    self.pop_temps();
                }
                self.close("}");
                self.unhoist(hoisted);
                self.pop_scope();
                self.close("}");
            }
            StmtKind::ForOf { name, name_span, iter, body, .. } => {
                let elem = self.facts(m).bindings.get(&s).copied().unwrap_or(ERROR);
                let elem = self.inst(elem);
                let cont = self.fresh("cont");
                self.open("{");
                self.push_scope();
                self.push_temps();
                let coll = self.expr(*iter);
                let coll_ty = coll.ty;
                // Iterate a snapshot (retained), so the loop is unaffected by mutation of the collection.
                let snap = self.consume(coll);
                let ct = self.ctype(coll_ty);
                let sn = self.fresh("it");
                self.line(format!("{ct} {sn} = {snap};"));
                self.pop_temps();
                let rel = format!("{};", self.release_code(coll_ty, &sn));
                self.b().scopes.last_mut().unwrap().releases.push(rel);
                let ect = self.ctype(elem);
                let uniq = self.fresh("");
                let var = format!("v_{}_{}", self.sym(*name), uniq);
                match self.tget(coll_ty) {
                    Ty::Set(_) => {
                        let kd = self.desc(elem);
                        let cur = self.fresh("i");
                        self.line(format!("bm_int {cur} = 0; void *{cur}k, *{cur}v;"));
                        self.open(&format!("while (bm_map_next({sn}, {kd}, &bm_type_undefined, &{cur}, &{cur}k, &{cur}v)) {{"));
                        self.line(format!("{ect} {var} = *({ect} *){cur}k;"));
                    }
                    _ => {
                        let idx = self.fresh("i");
                        self.open(&format!("for (bm_int {idx} = 0; {idx} < bm_arr_len({sn}); {idx}++) {{"));
                        self.line(format!("{ect} {var} = (({ect} *)bm_arr_data({sn}))[{idx}];"));
                    }
                }
                self.b().locals.insert(name_span.start, super::body::Local { access: var, ty: elem });
                let hoisted = self.hoist_unique(&[*body], &[]);
                self.loop_body(*body, &cont);
                self.unhoist(hoisted);
                self.line(format!("{cont}:;"));
                self.close("}");
                self.pop_scope();
                self.close("}");
            }
            StmtKind::Switch(disc, cases) => self.switch(s, *disc, cases),
            StmtKind::Throw(x) => {
                self.push_temps();
                let v = self.expr(*x);
                let code = self.consume(v);
                self.line(format!("bmg_err = (void *)({code});"));
                self.error_path();
                self.pop_temps_silent();
            }
            StmtKind::Try { body, catch, finally } => self.try_stmt(s, *body, catch.as_ref(), *finally),
            StmtKind::Return(v) => {
                let ret = self.b().ret;
                match v {
                    Some(x) if ret != VOID => {
                        self.push_temps();
                        let val = self.expr(*x);
                        let val = self.coerce(val, ret);
                        let code = self.consume(val);
                        let ct = self.ctype(ret);
                        let r = self.fresh("r");
                        self.line(format!("{ct} {r} = {code};"));
                        self.release_all_temps();
                        self.unwind_for_exit(0);
                        self.line(format!("return {r};"));
                        // The statement's temp scope is gone on this path; keep structure balanced.
                        self.b().temps.pop();
                    }
                    Some(x) => {
                        self.push_temps();
                        let _ = self.expr(*x);
                        self.pop_temps();
                        self.unwind_for_exit(0);
                        self.line("return;");
                    }
                    None => {
                        if ret == VOID {
                            self.unwind_for_exit(0);
                            self.line("return;");
                        } else {
                            let v = self.coerce(Val::plain("0", UNDEFINED), ret);
                            let ct = self.ctype(ret);
                            let r = self.fresh("r");
                            self.line(format!("{ct} {r} = {};", v.code));
                            self.unwind_for_exit(0);
                            self.line(format!("return {r};"));
                        }
                    }
                }
            }
            StmtKind::Break => {
                let (target, depth) = self.b().breaks.last().cloned().expect("break target");
                self.unwind_for_exit(depth);
                match target {
                    BreakTarget::Native => self.line("break;"),
                    BreakTarget::Goto(l) => self.line(format!("goto {l};")),
                }
            }
            StmtKind::Continue => {
                let (label, depth) = self.b().loops.last().cloned().expect("continue target");
                self.unwind_for_exit(depth);
                self.line(format!("goto {label};"));
            }
        }
    }

    /// Drops the current temp scope without emitting releases (after a path that already left).
    fn pop_temps_silent(&mut self) {
        self.b().temps.pop();
    }

    /// `try { } catch (e) { } finally { }`: errors in the block jump to the handler label.
    fn try_stmt(&mut self, s: StmtId, body: StmtId, catch: Option<&ast::Catch>, finally: Option<StmtId>) {
        let m = self.cur_m();
        let (lcatch, lend, lfin) = (self.fresh("catch"), self.fresh("tryend"), self.fresh("finerr"));
        let pend = finally.map(|_| {
            let p = self.fresh("pend");
            self.line(format!("void *{p} = NULL;"));
            p
        });
        let depth = self.b().scopes.len();
        if let Some(f) = finally {
            self.b().finallies.push((f, depth));
        }
        self.b().handlers.push((lcatch.clone(), depth));
        self.scoped_block(body);
        self.b().handlers.pop();
        self.line(format!("goto {lend};"));
        self.line(format!("{lcatch}:;"));
        match catch {
            Some(c) => {
                self.open("{");
                self.push_scope();
                let ev = self.fresh("err");
                self.line(format!("void *{ev} = bmg_err; bmg_err = NULL;"));
                match &c.param {
                    Some((name, nspan, _)) => {
                        let ty = self.facts(m).bindings.get(&s).copied().unwrap_or(ERROR);
                        let ty = self.inst(ty);
                        let ct = self.ctype(ty);
                        let uniq = self.fresh("");
                        let var = format!("v_{}_{}", self.sym(*name), uniq);
                        self.line(format!("{ct} {var} = ({ct}){ev};"));
                        let rel = format!("{};", self.release_code(ty, &var));
                        self.b().scopes.last_mut().unwrap().releases.push(rel);
                        self.b().locals.insert(nspan.start, Local { access: var, ty });
                    }
                    None => self.line(format!("bmg_obj_release({ev});")),
                }
                if finally.is_some() {
                    self.b().handlers.push((lfin.clone(), depth));
                }
                let cm = self.cur_m();
                if let StmtKind::Block(ss) = &self.ast(cm).stmt(c.body).kind {
                    for &x in ss {
                        self.stmt(x);
                    }
                } else {
                    self.stmt(c.body);
                }
                if finally.is_some() {
                    self.b().handlers.pop();
                }
                self.pop_scope();
                self.close("}");
                self.line(format!("goto {lend};"));
                if let Some(p) = &pend {
                    self.line(format!("{lfin}:; {p} = bmg_err; bmg_err = NULL;"));
                }
            }
            None => {
                if let Some(p) = &pend {
                    self.line(format!("{p} = bmg_err; bmg_err = NULL;"));
                }
            }
        }
        if finally.is_some() {
            self.b().finallies.pop();
        }
        self.line(format!("{lend}:;"));
        if let (Some(f), Some(p)) = (finally, pend) {
            self.scoped_block(f);
            self.open(&format!("if ({p} != NULL) {{"));
            self.line(format!("bmg_err = {p};"));
            self.error_path();
            self.close("}");
        }
    }

    /// A block statement in its own C block and scope.
    fn scoped_block(&mut self, s: StmtId) {
        self.open("{");
        self.scoped_stmt(s);
        self.close("}");
    }

    /// A statement in its own scope (branches of `if`).
    fn scoped_stmt(&mut self, s: StmtId) {
        self.push_scope();
        let m = self.cur_m();
        if let StmtKind::Block(ss) = &self.ast(m).stmt(s).kind {
            for &x in ss {
                self.stmt(x);
            }
        } else {
            self.stmt(s);
        }
        self.pop_scope();
    }

    fn cond_break(&mut self, c: ExprId) {
        self.push_temps();
        let cond = self.cond(c);
        let cv = self.fresh("c");
        self.line(format!("bool {cv} = {cond};"));
        self.pop_temps();
        self.line(format!("if (!{cv}) break;"));
    }

    fn loop_body(&mut self, body: StmtId, cont: &str) {
        self.push_scope();
        let depth = self.b().scopes.len();
        self.b().loops.push((cont.to_string(), depth));
        self.b().breaks.push((BreakTarget::Native, depth - 1));
        let m = self.cur_m();
        if let StmtKind::Block(ss) = &self.ast(m).stmt(body).kind {
            for &x in ss {
                self.stmt(x);
            }
        } else {
            self.stmt(body);
        }
        self.b().loops.pop();
        self.b().breaks.pop();
        self.pop_scope();
    }

    fn switch(&mut self, _sid: StmtId, disc: ExprId, cases: &[ast::Case]) {
        let m = self.cur_m();
        let ast = self.ast(m);
        // Discriminated (`switch (x.kind)` on a union of records) or literal (`switch (x)` on literal unions):
        // a C switch on the tag. Anything else: an if-chain.
        self.push_temps();
        let (tag_expr, label_tag): (Option<String>, LabelTag) = {
            let mut result: (Option<String>, LabelTag) = (None, Box::new(|_, _| None));
            if let ExprKind::Member { obj, name, optional: false, .. } = &ast.expr(disc).kind {
                let base = self.expr(*obj);
                let base = self.unfold_val(base);
                if let Ty::Union(ms) = self.tget(base.ty) {
                    let ms = self.c.types.tys(ms).to_vec();
                    let field = *name;
                    let mut map: Vec<(crate::intern::Sym, usize)> = Vec::new();
                    let mut ok = true;
                    for (i, &mt) in ms.iter().enumerate() {
                        let mt2 = self.c.unfold(mt);
                        match self.tget(mt2) {
                            Ty::Record(fs) => match self.c.types.fields(fs).iter().find(|f| f.name == field).map(|f| *self.c.types.get(f.ty)) {
                                Some(Ty::StrLit(l)) => map.push((l, i)),
                                _ => ok = false,
                            },
                            _ => ok = false,
                        }
                    }
                    if ok {
                        result = (Some(self.u_tag(&base.code, base.ty)), Box::new(move |_, l| map.iter().find(|(s, _)| *s == l).map(|(_, i)| *i)));
                    }
                }
            }
            if result.0.is_none() {
                let v = self.expr(disc);
                if let Ty::Union(ms) = self.tget(v.ty) {
                    let ms = self.c.types.tys(ms).to_vec();
                    if ms.iter().all(|&x| matches!(self.tget(x), Ty::StrLit(_))) {
                        let lits: Vec<(crate::intern::Sym, usize)> = ms.iter().enumerate().map(|(i, &x)| (if let Ty::StrLit(l) = self.tget(x) { l } else { unreachable!() }, i)).collect();
                        result = (Some(self.u_tag(&v.code, v.ty)), Box::new(move |_, l| lits.iter().find(|(s, _)| *s == l).map(|(_, i)| *i)));
                    }
                }
                if result.0.is_none() {
                    // General: keep the value for the if-chain.
                    let v = self.own(v);
                    let ct = self.ctype(v.ty);
                    let dv = self.fresh("d");
                    self.line(format!("{ct} {dv} = {};", v.code));
                    self.pop_temps();
                    self.if_chain_switch(&dv, v.ty, cases, v.owned);
                    return;
                }
            }
            result
        };
        let tag_expr = tag_expr.unwrap();
        let tv = self.fresh("tag");
        self.line(format!("uint32_t {tv} = {tag_expr};"));
        self.pop_temps();
        self.open(&format!("switch ({tv}) {{"));
        let depth = self.b().scopes.len();
        self.b().breaks.push((BreakTarget::Native, depth));
        for case in cases {
            match case.test {
                Some(t) => match &ast.expr(t).kind {
                    ExprKind::Str(l) => match label_tag(self, *l) {
                        Some(i) => self.line(format!("case {i}:")),
                        None => self.line("case 0xFFFFFFFF:"),
                    },
                    _ => self.line("case 0xFFFFFFFF:"),
                },
                None => self.line("default:"),
            }
            if !case.body.is_empty() {
                self.open("{");
                self.push_scope();
                for &x in &case.body {
                    self.stmt(x);
                }
                self.pop_scope();
                self.close("}");
                self.line("break;");
            }
        }
        self.b().breaks.pop();
        self.close("}");
    }

    fn if_chain_switch(&mut self, dv: &str, dty: TyId, cases: &[ast::Case], owned: bool) {
        let end = self.fresh("swend");
        let depth = self.b().scopes.len();
        self.b().breaks.push((BreakTarget::Goto(end.clone()), depth));
        // Match: find the first matching case (or default), then run bodies from there (grouped labels).
        let target = self.fresh("sw");
        self.line(format!("int {target} = -1;"));
        let mut default_idx = None;
        for (i, case) in cases.iter().enumerate() {
            match case.test {
                Some(t) => {
                    self.push_temps();
                    let v = self.expr(t);
                    let eq = self.equality(Val::plain(dv, dty), v, false);
                    self.line(format!("if ({target} < 0 && ({eq})) {target} = {i};"));
                    self.pop_temps();
                }
                None => default_idx = Some(i),
            }
        }
        if let Some(d) = default_idx {
            self.line(format!("if ({target} < 0) {target} = {d};"));
        }
        self.open(&format!("switch ({target}) {{"));
        for (i, case) in cases.iter().enumerate() {
            self.line(format!("case {i}:"));
            if !case.body.is_empty() {
                self.open("{");
                self.push_scope();
                for &x in &case.body {
                    self.stmt(x);
                }
                self.pop_scope();
                self.close("}");
                self.line(format!("goto {end};"));
            }
        }
        self.line("default: break;");
        self.close("}");
        self.b().breaks.pop();
        self.line(format!("{end}:;"));
        if owned && self.is_rc(dty) {
            let r = self.release_code(dty, dv);
            self.line(format!("{r};"));
        }
    }

    // ------------------------------------------------------------ expressions

    /// Emits an expression; the value has the checker's (instantiated) type for it.
    pub(crate) fn expr(&mut self, e: ExprId) -> Val {
        let m = self.cur_m();
        let ast = self.ast(m);
        let node = ast.expr(e);
        let span = node.span;
        let ty = self.ty(e);
        match &node.kind {
            ExprKind::Error => Val::plain("0", ERROR),
            ExprKind::Int(v) => {
                let code = match self.tget(ty) {
                    Ty::F64 | Ty::F32 => format!("{}.0", v),
                    Ty::U64 => format!("UINT64_C({v})"),
                    _ => format!("INT64_C({v})"),
                };
                let code = if matches!(self.tget(ty), Ty::F64 | Ty::F32 | Ty::U64 | Ty::Int) { code } else { format!("(({}){code})", self.ctype(ty)) };
                Val::plain(code, ty)
            }
            ExprKind::Float(f) => {
                let mut s = format!("{f:?}");
                if !s.contains('.') && !s.contains('e') && !s.contains("inf") && !s.contains("NaN") {
                    s.push_str(".0");
                }
                Val::plain(if ty == F32 { format!("{s}f") } else { s }, ty)
            }
            ExprKind::Str(_) | ExprKind::Undefined | ExprKind::Null => Val::plain("0", ty),
            ExprKind::Bool(b) => Val::plain(if *b { "true" } else { "false" }, BOOL),
            ExprKind::Template(parts, exprs) => {
                let vals: Vec<Val> = exprs.iter().map(|&x| self.expr(x)).collect();
                let sbv = self.fresh("sb");
                self.line(format!("bm_sb {sbv} = {{0}};"));
                self.open("{");
                self.line(format!("bm_sb *sb = &{sbv};"));
                for (i, part) in parts.iter().enumerate() {
                    if !part.is_empty() {
                        self.line(format!("bm_sb_push(sb, {}, {});", c_string(part.as_bytes()), part.len()));
                    }
                    if let Some(v) = vals.get(i) {
                        let code = self.string_code(v.ty, &v.code);
                        self.line(format!("{code};"));
                    }
                }
                self.close("}");
                self.tmp(STR, &format!("bm_str_from_sb(&{sbv})"), true)
            }
            ExprKind::Ident(_) => self.ident(e, ty),
            ExprKind::Paren(x) => self.expr(*x),
            ExprKind::Unary(op, x) => self.unary(*op, *x, ty, span),
            ExprKind::Binary(op, l, r) => self.binary(*op, *l, *r, ty, span),
            ExprKind::Assign(op, t, v) => self.assign(*op, *t, *v, span),
            ExprKind::Update { inc, prefix, target } => {
                let p = self.place(*target);
                let ct = self.ctype(p.ty);
                let loc = self.loc(span);
                let pv = self.fresh("p");
                self.line(format!("{ct} *{pv} = &{};", p.lv));
                let old = self.fresh("o");
                self.line(format!("{ct} {old} = *{pv};"));
                let new = if self.c.types.is_float(p.ty) {
                    format!("{old} {} 1", if *inc { "+" } else { "-" })
                } else {
                    format!("{}({ct}, {old}, 1, {loc})", if *inc { "BM_ADD" } else { "BM_SUB" })
                };
                self.line(format!("*{pv} = {new};"));
                Val::plain(if *prefix { format!("(*{pv})") } else { old }, p.ty)
            }
            ExprKind::Call { .. } => self.call(e),
            ExprKind::New { callee, args, .. } => {
                if let Some(fact) = self.facts(m).calls.get(&e).cloned()
                    && let Callee::New(_) = fact.callee
                {
                    let params: Vec<FnParam> = fact.params.iter().map(|p| FnParam { ty: self.inst(p.ty), ..*p }).collect();
                    let cls = self.inst(fact.ret);
                    let v = self.new_object(e, cls, args, &params, span);
                    if self.facts(m).throwing.contains(&e) {
                        self.error_check();
                    }
                    return self.coerce(v, ty);
                }
                if let Some(fact) = self.facts(m).calls.get(&e)
                    && let Callee::NewPromise = fact.callee
                {
                    self.unsupported(span, "`new Promise`");
                    return Val::plain("0", ty);
                }
                let name = match &ast.expr(*callee).kind {
                    ExprKind::Ident(s) => self.sym(*s).to_string(),
                    _ => String::new(),
                };
                if name == "Set" && !args.is_empty() {
                    let Ty::Set(elem) = self.tget(ty) else { return Val::plain("BM_EMPTY_MAP", ty) };
                    let arr = self.expr(args[0].expr);
                    let arr_ty = self.c.types.array(elem);
                    let arr = self.coerce(arr, arr_ty);
                    let res = self.tmp(ty, "BM_EMPTY_MAP", true);
                    let (d, ect) = (self.desc(elem), self.ctype(elem));
                    let i = self.fresh("i");
                    self.open(&format!("for (bm_int {i} = 0; {i} < bm_arr_len({}); {i}++) {{", arr.code));
                    let r = self.retain_code(elem, &format!("{i}k"));
                    self.line(format!("{ect} {i}k = (({ect} *)bm_arr_data({}))[{i}]; {r};", arr.code));
                    self.line(format!("bm_map_set(&{}, {d}, &bm_type_undefined, &{i}k, NULL);", res.code));
                    self.close("}");
                    return res;
                }
                Val::plain("BM_EMPTY_MAP", ty)
            }
            ExprKind::Member { obj, name, optional, .. } => self.member(e, *obj, *name, *optional, ty, span),
            ExprKind::Index { obj, index, optional } => self.index(*obj, *index, *optional, ty, span),
            ExprKind::Object(fields) => {
                let target = self.c.unfold(ty);
                // Checked against an interface: build a record with the interface's members, then wrap it.
                let target = if let Ty::Interface(..) = self.tget(target) {
                    let ms = self.iface_members(target);
                    let order: Vec<crate::intern::Sym> = ms.iter().map(|f| f.name).collect();
                    let rt = self.c.types.record(ms);
                    self.c.types.note_field_order(rt, order);
                    rt
                } else {
                    target
                };
                // A `Record<string, V>` literal: a map built key by key.
                if let Ty::Map(k, v) = self.tget(target) {
                    let res = self.tmp(target, "BM_EMPTY_MAP", true);
                    let (kd, vd) = (self.desc(k), self.desc(v));
                    let (kct, vct) = (self.ctype(k), self.ctype(v));
                    for f in fields {
                        let key = self.lit(self.sym(f.name).to_string().as_bytes());
                        let vv = self.expr(f.value);
                        let vv = self.coerce(vv, v);
                        let vc = self.consume(vv);
                        let (kn, vn) = (self.fresh("k"), self.fresh("v"));
                        self.line(format!("{kct} {kn} = {key}; {vct} {vn} = {vc};"));
                        self.line(format!("bm_map_set(&{}, {kd}, {vd}, &{kn}, &{vn});", res.code));
                    }
                    return self.coerce(res, ty);
                }
                let Ty::Record(fs) = self.tget(target) else {
                    self.unsupported(span, "an object literal of this type");
                    return Val::plain("0", ty);
                };
                let tfields = self.c.types.fields(fs).to_vec();
                let mut inits: Vec<(crate::intern::Sym, String)> = Vec::new();
                for f in fields {
                    let fty = tfields.iter().find(|x| x.name == f.name).map(|x| x.ty).unwrap_or(ERROR);
                    let v = self.expr(f.value);
                    let v = self.coerce(v, fty);
                    let code = self.consume(v);
                    inits.push((f.name, code));
                }
                for tf in &tfields {
                    if !inits.iter().any(|(n, _)| *n == tf.name) {
                        let v = self.coerce(Val::plain("0", UNDEFINED), tf.ty);
                        let code = self.consume(v);
                        inits.push((tf.name, code));
                    }
                }
                let ct = self.ctype(target);
                let parts: Vec<String> = inits.iter().map(|(n, c)| format!(".f_{} = {c}", self.sym(*n))).collect();
                let v = self.tmp(target, &format!("({ct}){{ {} }}", parts.join(", ")), true);
                if target != ty {
                    return self.coerce(v, ty);
                }
                v
            }
            ExprKind::Array(elems) => {
                let Ty::Array(et) = self.tget(ty) else {
                    self.unsupported(span, "an array literal of this type");
                    return Val::plain("BM_EMPTY_ARR", ty);
                };
                if elems.is_empty() {
                    return Val::plain("BM_EMPTY_ARR", ty);
                }
                // Evaluate the elements, then write them straight into reserved space: the length
                // is a constant the C compiler can see.
                let d = self.desc(et);
                let ect = self.ctype(et);
                let mut codes = Vec::new();
                for &x in elems {
                    let v = self.expr(x);
                    let v = self.coerce(v, et);
                    let code = self.consume(v);
                    let n = self.fresh("e");
                    self.line(format!("{ect} {n} = {code};"));
                    codes.push(n);
                }
                let arr = self.tmp(ty, "BM_EMPTY_ARR", true);
                let w = self.fresh("w");
                self.line(format!("{ect} *{w} = ({ect} *)bm_arr_reserve_tail(&{}, {d}, {});", arr.code, codes.len()));
                for (i, c) in codes.iter().enumerate() {
                    self.line(format!("{w}[{i}] = {c};"));
                }
                // A fresh literal starts empty, so its length is exactly the element count.
                self.line(format!("{}.len = {};", arr.code, codes.len()));
                arr
            }
            ExprKind::Arrow(_) => self.closure_value(e, ty),
            ExprKind::Cond(c, a, b) => {
                let cond = self.cond(*c);
                let res = self.fresh("t");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                self.open(&format!("if ({cond}) {{"));
                self.push_temps();
                let va = self.expr(*a);
                let va = self.coerce(va, ty);
                let code = self.consume(va);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("} else {");
                self.b().indent += 1;
                self.push_temps();
                let vb = self.expr(*b);
                let vb = self.coerce(vb, ty);
                let code = self.consume(vb);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            ExprKind::As(x, _) => {
                let v = self.expr(*x);
                self.downcast(v, ty, span)
            }
            ExprKind::NonNull(x) => {
                // `xs[i]!`: a direct bounds-checked read.
                if let ExprKind::Index { obj, index, optional: false } = &ast.expr(*x).kind {
                    let ot = self.ty(*obj);
                    if let Ty::Array(et) = self.tget(ot) {
                        let a = self.expr(*obj);
                        let i = self.expr(*index);
                        let i = self.int_code(i);
                        let loc = self.loc(span);
                        let ect = self.ctype(et);
                        let v = Val::plain(format!("(*({ect} *)bm_arr_at({}, sizeof({ect}), {i}, {loc}))", a.code), et);
                        return self.coerce(v, ty);
                    }
                }
                let v = self.expr(*x);
                self.downcast(v, ty, span)
            }
            ExprKind::Typeof(x) => {
                let v = self.expr(*x);
                self.typeof_str(v)
            }
            ExprKind::This => self.ident(e, ty),
            ExprKind::Try(x) => self.expr(*x),
            // Synchronous until real async is on (BARM_ASYNC): the value itself.
            ExprKind::Await(x) => self.expr(*x),
            ExprKind::Super => Val::plain("0", ty),
        }
    }

    fn ident(&mut self, e: ExprId, ty: TyId) -> Val {
        let m = self.cur_m();
        match self.ident_fact(m, e) {
            Some(IdentFact::Local(k)) => {
                let Some(local) = self.b().locals.get(&k).cloned() else {
                    let span = self.expr_span(e);
                    self.unsupported(span, "this variable reference");
                    return Val::plain("0", ty);
                };
                self.project(Val::plain(local.access, local.ty), ty)
            }
            Some(IdentFact::Fn(fm, fi)) => {
                let code = self.fn_value(fm, fi);
                Val::plain(code, ty)
            }
            Some(IdentFact::Const(cm, ci)) => {
                let v = self.const_ref(cm, ci);
                self.project(v, ty)
            }
            _ => Val::plain("0", ty),
        }
    }

    /// Unfolds a recursive-alias value to its underlying (borrowed) value.
    pub(crate) fn unfold_val(&mut self, v: Val) -> Val {
        if matches!(self.tget(v.ty), Ty::Rec(..)) {
            let inner = self.c.unfold(v.ty);
            return Val { code: format!("({})->v", v.code), ty: inner, owned: false };
        }
        v
    }

    /// Views a value of a declared type as a narrower (flow-narrowed) type.
    pub(crate) fn project(&mut self, v: Val, to: TyId) -> Val {
        if v.ty == to || to == ERROR {
            return v;
        }
        // A string narrowed to a literal (`if (path === "/")`): the value is known.
        if matches!(self.tget(to), Ty::StrLit(_)) && matches!(self.tget(v.ty), Ty::Str) {
            let d = self.default_value(to);
            return Val::plain(d, to);
        }
        // Narrowed by `instanceof`: a class viewed as a subclass (or a union member downcast).
        if let Ty::Class(tc, _) = self.tget(to) {
            match self.tget(v.ty) {
                Ty::Class(..) => return self.class_cast(v, to),
                Ty::Union(ms) => {
                    let ms = self.c.types.tys(ms).to_vec();
                    if let Some(k) = ms.iter().position(|&m| matches!(self.tget(m), Ty::Class(mc, _) if self.c.class_descends(tc, mc))) {
                        let payload = Val { code: self.u_payload(&v.code, v.ty, k), ty: ms[k], owned: false };
                        return self.class_cast(payload, to);
                    }
                }
                _ => {}
            }
        }
        if matches!(self.tget(v.ty), Ty::Rec(..)) {
            let u = self.unfold_val(v);
            return self.project(u, to);
        }
        if let Ty::Union(_) = self.tget(v.ty) {
            if let Some(k) = self.tag_of(v.ty, to) {
                if self.is_unit(to) {
                    return Val::plain("0", to);
                }
                return Val { code: self.u_payload(&v.code, v.ty, k), ty: to, owned: false };
            }
            // Narrowed to a smaller union: re-tag (members outside the target can't occur here).
            if let Ty::Union(fms) = self.tget(v.ty)
                && let Ty::Union(_) = self.tget(to)
            {
                let fms = self.c.types.tys(fms).to_vec();
                return self.retag(v, &fms, to);
            }
            return self.coerce(v, to);
        }
        self.coerce(v, to)
    }

    // ------------------------------------------------------------ conversions

    /// Converts `v` to type `to` (widening, union injection, sub-union retagging, boxing).
    pub(crate) fn coerce(&mut self, v: Val, to: TyId) -> Val {
        self.coerce_inner(v, to)
    }

    fn coerce_inner(&mut self, v: Val, to: TyId) -> Val {
        let from = v.ty;
        if from == to || to == ERROR || from == ERROR || from == NEVER {
            return Val { ty: to, ..v };
        }
        let (tf, tt) = (self.tget(from), self.tget(to));
        match (tf, tt) {
            (Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64, Ty::F64) => return Val::plain(format!("((double)({}))", v.code), to),
            (Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 | Ty::F64, Ty::F32) => return Val::plain(format!("((float)({}))", v.code), to),
            (Ty::F32, Ty::F64) => return Val::plain(format!("((double)({}))", v.code), to),
            (Ty::StrLit(s), Ty::Str) => {
                let text = self.sym(s).to_string();
                return Val::plain(self.lit(text.as_bytes()), STR);
            }
            (Ty::Undefined, Ty::Void) | (Ty::Void, Ty::Undefined) => return Val::plain("0", to),
            _ => {}
        }
        // Class to class (upcast): a pointer cast.
        if let (Ty::Class(..), Ty::Class(..)) = (tf, tt) {
            return self.class_cast(v, to);
        }
        // Into an interface value (a fat pointer).
        if let (Ty::Record(_) | Ty::Class(..) | Ty::Interface(..), Ty::Interface(..)) = (tf, tt)
            && let Some(iv) = self.wrap_iface(v.clone(), to)
        {
            return iv;
        }
        // Recursive aliases: box / unbox.
        if let Ty::Rec(..) = tt {
            let inner = self.c.unfold(to);
            let iv = self.coerce(v, inner);
            let code = self.consume(iv);
            let bn = self.box_name(to);
            let bv = self.fresh("bx");
            self.line(format!("{bn} *{bv} = bmg_alloc_small(sizeof({bn})); {bv}->rc = 1; {bv}->v = {code};"));
            let owned = true;
            self.b().temps.last_mut().unwrap().push((bv.clone(), to));
            return Val { code: bv, ty: to, owned };
        }
        // Into a union.
        if let Ty::Union(_) = tt {
            let members = self.members(to);
            // Union → union: retag each member.
            if let Ty::Union(fms) = tf {
                let fms = self.c.types.tys(fms).to_vec();
                if fms.iter().all(|&fm| members.contains(&fm) || self.member_target(to, fm).is_some()) {
                    return self.retag(v, &fms, to);
                }
            }
            if let Some(k) = members.iter().position(|&mt| mt == from) {
                if self.is_unit(from) {
                    let u = self.u_make(to, k, None);
                    return self.tmp(to, &u, false);
                }
                let code = self.consume(v);
                let u = self.u_make(to, k, Some(&code));
                return self.tmp(to, &u, true);
            }
            if let Some(mt) = self.member_target(to, from) {
                let mv = self.coerce(v, mt);
                return self.coerce(mv, to);
            }
        }
        // Out of a recursive alias (after union injection, so `Tree` → `Tree | undefined` doesn't unbox).
        if let Ty::Rec(..) = tf {
            let u = self.unfold_val(v);
            return self.coerce(u, to);
        }
        // Union of string literals → string.
        if let (Ty::Union(fms), Ty::Str) = (tf, tt) {
            let fms = self.c.types.tys(fms).to_vec();
            let res = self.fresh("s");
            self.line(format!("bm_str {res};"));
            self.open(&format!("switch (({}).tag) {{", v.code));
            for (i, &fm) in fms.iter().enumerate() {
                let s = match self.tget(fm) {
                    Ty::StrLit(l) => {
                        let text = self.sym(l).to_string();
                        self.lit(text.as_bytes())
                    }
                    Ty::Str => format!("({}).u.m{i}", v.code),
                    _ => "BM_EMPTY_STR".into(),
                };
                self.line(format!("case {i}: {res} = {s}; break;"));
            }
            self.line(format!("default: {res} = BM_EMPTY_STR; break;"));
            self.close("}");
            return Val::plain(res, STR);
        }
        // Structural conversions between records / arrays with assignable parts.
        match (tf, tt) {
            (Ty::Record(ffs), Ty::Record(tfs)) => {
                let (ffs, tfs) = (self.c.types.fields(ffs).to_vec(), self.c.types.fields(tfs).to_vec());
                let mut parts = Vec::new();
                for (ff, tfld) in ffs.iter().zip(&tfs) {
                    let fv = Val { code: format!("({}).f_{}", v.code, self.sym(ff.name)), ty: ff.ty, owned: false };
                    let cv = self.coerce(fv, tfld.ty);
                    let code = self.consume(cv);
                    parts.push(format!(".f_{} = {code}", self.sym(tfld.name)));
                }
                let ct = self.ctype(to);
                return self.tmp(to, &format!("({ct}){{ {} }}", parts.join(", ")), true);
            }
            (Ty::Array(fe), Ty::Array(te)) => {
                let d = self.desc(te);
                let (fct, tct) = (self.ctype(fe), self.ctype(te));
                let res = self.tmp(to, &format!("bm_arr_with_capacity({d}, bm_arr_len({}))", v.code), true);
                let i = self.fresh("i");
                self.open(&format!("for (bm_int {i} = 0; {i} < bm_arr_len({}); {i}++) {{", v.code));
                self.push_temps();
                let ev = Val::plain(format!("(({fct} *)bm_arr_data({}))[{i}]", v.code), fe);
                let cv = self.coerce(ev, te);
                let code = self.consume(cv);
                self.line(format!("BMG_PUSH({tct}, &{}, {d}, {code});", res.code));
                self.pop_temps();
                self.close("}");
                return res;
            }
            (Ty::Func(..), Ty::Func(..)) => {
                // Same C calling convention when parameter and return C types agree (e.g. a result ignored as void).
                return Val { ty: to, ..v };
            }
            _ => {}
        }
        let (fs, ts) = (self.c.show(from), self.c.show(to));
        self.internal.push(format!("no conversion from `{fs}` to `{ts}`"));
        Val { ty: to, ..v }
    }

    /// The member of union `to` that a value of type `from` converts into.
    fn member_target(&mut self, to: TyId, from: TyId) -> Option<TyId> {
        let members = self.members(to);
        if members.contains(&from) {
            return Some(from);
        }
        members.into_iter().find(|&mt| mt != UNDEFINED && self.c.assignable_pub(from, mt))
    }

    /// Converts a union value to another union whose members accept every source member.
    fn retag(&mut self, v: Val, fms: &[TyId], to: TyId) -> Val {
        let ct = self.ctype(to);
        let res = self.fresh("u");
        self.line(format!("{ct} {res};"));
        let tag = self.u_tag(&v.code, v.ty);
        self.open(&format!("switch ({tag}) {{"));
        for (i, &fm) in fms.iter().enumerate() {
            let target = self.member_target(to, fm).or_else(|| {
                // After `instanceof`: a base-class member becomes a subclass member.
                let Ty::Class(fc, _) = self.tget(fm) else { return None };
                self.members(to).into_iter().find(|&t| matches!(self.tget(t), Ty::Class(tc, _) if self.c.class_descends(tc, fc)))
            });
            let Some(mt) = target else { continue };
            let k = self.tag_of(to, mt).unwrap();
            self.open(&format!("case {i}: {{"));
            self.push_temps();
            if self.is_unit(mt) {
                let u = self.u_make(to, k, None);
                self.line(format!("{res} = {u};"));
            } else {
                let payload = if self.is_unit(fm) { Val::plain("0", fm) } else { Val::plain(self.u_payload(&v.code, v.ty, i), fm) };
                let cv = if matches!((self.tget(fm), self.tget(mt)), (Ty::Class(..), Ty::Class(..))) { self.class_cast(payload, mt) } else { self.coerce(payload, mt) };
                let code = self.consume(cv);
                let u = self.u_make(to, k, Some(&code));
                self.line(format!("{res} = {u};"));
            }
            self.pop_temps();
            self.line("break;");
            self.close("}");
        }
        let dv = self.default_value(to);
        self.line(format!("default: {res} = {dv}; break;"));
        self.close("}");
        let owned = self.is_rc(to);
        if owned {
            self.b().temps.last_mut().unwrap().push((res.clone(), to));
        }
        Val { code: res, ty: to, owned }
    }

    /// `x!` / `x as T`: checks the tag at run time, then projects.
    fn downcast(&mut self, v: Val, to: TyId, span: Span) -> Val {
        if v.ty == to {
            return v;
        }
        if let (Ty::Class(fc, _), Ty::Class(tc, _)) = (self.tget(v.ty), self.tget(to))
            && !self.c.class_descends(fc, tc)
        {
            let isa = self.instanceof_code(v.clone(), tc);
            let loc = self.loc(span);
            self.line(format!("if (!{isa}) bm_trap(\"`as` cast failed: the object is a different class\", {loc});"));
            return self.class_cast(v, to);
        }
        let v = if matches!(self.tget(v.ty), Ty::Rec(..)) { self.unfold_val(v) } else { v };
        if let Ty::Union(_) = self.tget(v.ty) {
            let fms = self.members(v.ty);
            let allowed: Vec<usize> = fms.iter().enumerate().filter(|(_, fm)| **fm == to || self.members(to).contains(fm)).map(|(i, _)| i).collect();
            if !allowed.is_empty() {
                let loc = self.loc(span);
                let conds: Vec<String> = allowed.iter().map(|&i| format!("!{}", self.u_is(&v.code, v.ty, i))).collect();
                let msg = if to == self.c.types.without_undefined(v.ty) { "value is undefined (`!` assertion failed)" } else { "`as` cast failed: the value has a different type" };
                self.line(format!("if ({}) bm_trap({}, {loc});", conds.join(" && "), c_string(msg.as_bytes())));
                return self.project(v, to);
            }
        }
        self.coerce(v, to)
    }

    fn typeof_str(&mut self, v: Val) -> Val {
        let name = |types: &Types, t: TyId| -> &'static str {
            if types.is_numeric(t) {
                "number"
            } else if types.is_string(t) {
                "string"
            } else if t == BOOL {
                "boolean"
            } else if t == UNDEFINED {
                "undefined"
            } else if matches!(types.get(t), Ty::Func(..)) {
                "function"
            } else {
                "object"
            }
        };
        if let Ty::Union(ms) = self.tget(v.ty) {
            let ms = self.c.types.tys(ms).to_vec();
            let res = self.fresh("s");
            self.line(format!("bm_str {res};"));
            let tag = self.u_tag(&v.code, v.ty);
            self.open(&format!("switch ({tag}) {{"));
            for (i, &m) in ms.iter().enumerate() {
                let n = name(&self.c.types, m);
                let l = self.lit(n.as_bytes());
                self.line(format!("case {i}: {res} = {l}; break;"));
            }
            self.line(format!("default: {res} = BM_EMPTY_STR; break;"));
            self.close("}");
            return Val::plain(res, STR);
        }
        let n = name(&self.c.types, v.ty);
        Val::plain(self.lit(n.as_bytes()), STR)
    }

    pub(crate) fn int_code(&mut self, v: Val) -> String {
        if v.ty == INT { v.code } else { format!("((bm_int)({}))", v.code) }
    }

    /// A C boolean for a condition (`bool`, or an optional value tested for presence).
    pub(crate) fn cond(&mut self, e: ExprId) -> String {
        let v = self.expr(e);
        self.truthy(v)
    }

    fn truthy(&mut self, v: Val) -> String {
        if v.ty == BOOL {
            return v.code;
        }
        let v = if matches!(self.tget(v.ty), Ty::Rec(..)) { return "true".into() } else { v };
        match self.tag_of(v.ty, UNDEFINED) {
            Some(k) => format!("(!{})", self.u_is(&v.code, v.ty, k)),
            None => "true".into(),
        }
    }

    // ------------------------------------------------------------ operators

    fn unary(&mut self, op: UnOp, x: ExprId, ty: TyId, span: Span) -> Val {
        match op {
            UnOp::Not => {
                let c = self.cond(x);
                Val::plain(format!("(!{c})"), BOOL)
            }
            UnOp::Neg => {
                let v = self.expr(x);
                let v = self.coerce(v, ty);
                if self.c.types.is_float(ty) {
                    Val::plain(format!("(-{})", v.code), ty)
                } else if v.code.starts_with("INT64_C(") || v.code.starts_with("((") {
                    // Literal: fold the sign (INT64_MIN is written as a literal too).
                    let ct = self.ctype(ty);
                    Val::plain(format!("(({ct})(-{}))", v.code), ty)
                } else {
                    let ct = self.ctype(ty);
                    let loc = self.loc(span);
                    Val::plain(format!("BM_NEG({ct}, {}, {loc})", v.code), ty)
                }
            }
            UnOp::Plus => {
                let v = self.expr(x);
                self.coerce(v, ty)
            }
            UnOp::BitNot => {
                let v = self.expr(x);
                Val::plain(format!("(~{})", v.code), ty)
            }
            UnOp::Void => {
                let _ = self.expr(x);
                Val::plain("0", UNDEFINED)
            }
        }
    }

    fn binary(&mut self, op: BinOp, l: ExprId, r: ExprId, ty: TyId, span: Span) -> Val {
        use BinOp::*;
        match op {
            And | Or => {
                let lc = self.cond(l);
                let res = self.fresh("b");
                self.line(format!("bool {res} = {lc};"));
                self.open(&format!("if ({}{res}) {{", if op == And { "" } else { "!" }));
                self.push_temps();
                let rc = self.cond(r);
                self.line(format!("{res} = {rc};"));
                self.pop_temps();
                self.close("}");
                Val::plain(res, BOOL)
            }
            Nullish => {
                let lv = self.expr(l);
                let res = self.fresh("t");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                let inner = self.c.types.without_undefined(lv.ty);
                let present = self.truthy(lv.clone());
                self.open(&format!("if ({present}) {{"));
                self.push_temps();
                let pv = self.project(lv, inner);
                let pv = self.coerce(pv, ty);
                let code = self.consume(pv);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("} else {");
                self.b().indent += 1;
                self.push_temps();
                let rv = self.expr(r);
                let rv = self.coerce(rv, ty);
                let code = self.consume(rv);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            Eq | Ne | LooseEq | LooseNe => {
                let lv = self.expr(l);
                let rv = self.expr(r);
                let eq = self.equality(lv, rv, true);
                Val::plain(if matches!(op, Eq | LooseEq) { eq } else { format!("(!{eq})") }, BOOL)
            }
            Lt | Gt | Le | Ge => {
                let lv = self.expr(l);
                let rv = self.expr(r);
                let o = op.as_str();
                if self.c.types.is_string(lv.ty) {
                    let (a, b) = (self.coerce(lv, STR), self.coerce(rv, STR));
                    return Val::plain(format!("(bm_str_cmp({}, {}) {o} 0)", a.code, b.code), BOOL);
                }
                let (a, b) = self.numeric_operands(lv, rv);
                Val::plain(format!("({} {o} {})", a, b), BOOL)
            }
            Add if ty == STR => {
                let lv = self.expr(l);
                let rv = self.expr(r);
                if lv.ty == STR && rv.ty == STR {
                    return self.tmp(STR, &format!("bm_str_concat({}, {})", lv.code, rv.code), true);
                }
                let sbv = self.fresh("sb");
                self.line(format!("bm_sb {sbv} = {{0}};"));
                self.open("{");
                self.line(format!("bm_sb *sb = &{sbv};"));
                for v in [lv, rv] {
                    let c = self.string_code(v.ty, &v.code);
                    self.line(format!("{c};"));
                }
                self.close("}");
                self.tmp(STR, &format!("bm_str_from_sb(&{sbv})"), true)
            }
            Add | Sub | Mul | Div | Rem | Pow => {
                let lv = self.expr(l);
                let rv = self.expr(r);
                self.arith(op, lv, rv, ty, span)
            }
            BitAnd | BitOr | BitXor | Shl | Shr | UShr => {
                let lv = self.expr(l);
                let rv = self.expr(r);
                let ct = self.ctype(ty);
                let (a, b) = (lv.code, rv.code);
                let code = match op {
                    BitAnd => format!("(({ct})({a} & {b}))"),
                    BitOr => format!("(({ct})({a} | {b}))"),
                    BitXor => format!("(({ct})({a} ^ {b}))"),
                    Shl => format!("(({ct})((uint64_t)({a}) << ((uint64_t)({b}) & 63)))"),
                    Shr => format!("(({ct})(({a}) >> ((uint64_t)({b}) & 63)))"),
                    _ => format!("(({ct})((uint64_t)({a}) >> ((uint64_t)({b}) & 63)))"),
                };
                Val::plain(code, ty)
            }
            Instanceof => {
                let v = self.expr(l);
                let m = self.cur_m();
                match self.ident_fact(m, r) {
                    Some(IdentFact::Class(c)) => {
                        let code = self.instanceof_code(v, c);
                        Val::plain(code, BOOL)
                    }
                    _ => Val::plain("false", BOOL),
                }
            }
            In => Val::plain("false", BOOL),
        }
    }

    fn numeric_operands(&mut self, a: Val, b: Val) -> (String, String) {
        if a.ty == b.ty {
            return (a.code, b.code);
        }
        let target = if self.c.types.is_float(a.ty) || self.c.types.is_float(b.ty) { F64 } else { INT };
        let a = self.coerce(a, target);
        let b = self.coerce(b, target);
        (a.code, b.code)
    }

    pub(crate) fn arith(&mut self, op: BinOp, lv: Val, rv: Val, ty: TyId, span: Span) -> Val {
        use BinOp::*;
        let ct = self.ctype(ty);
        if self.c.types.is_float(ty) {
            let a = self.coerce(lv, ty);
            let b = self.coerce(rv, ty);
            let code = match op {
                Add => format!("({} + {})", a.code, b.code),
                Sub => format!("({} - {})", a.code, b.code),
                Mul => format!("({} * {})", a.code, b.code),
                Div => format!("({} / {})", a.code, b.code),
                Rem => format!("fmod({}, {})", a.code, b.code),
                _ => format!("pow({}, {})", a.code, b.code),
            };
            return Val::plain(code, ty);
        }
        let loc = self.loc(span);
        let unsigned = matches!(self.tget(ty), Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64);
        let (a, b) = (lv.code, rv.code);
        let code = match op {
            Add => format!("BM_ADD({ct}, {a}, {b}, {loc})"),
            Sub => format!("BM_SUB({ct}, {a}, {b}, {loc})"),
            Mul => format!("BM_MUL({ct}, {a}, {b}, {loc})"),
            Rem if unsigned => format!("BM_UREM({ct}, {a}, {b}, {loc})"),
            Rem => format!("BM_REM({ct}, {a}, {b}, {loc})"),
            _ => format!("bm_pow_int({a}, {b}, {loc})"),
        };
        Val::plain(code, ty)
    }

    /// `a === b` as a C boolean.
    pub(crate) fn equality(&mut self, a: Val, b: Val, _strict: bool) -> String {
        let (ta, tb) = (a.ty, b.ty);
        if self.c.types.is_numeric(ta) && self.c.types.is_numeric(tb) {
            let (x, y) = self.numeric_operands(a, b);
            return format!("({x} == {y})");
        }
        if ta == tb {
            return self.eq_code(ta, &a.code, &b.code);
        }
        // Literal vs literal / string.
        match (self.tget(ta), self.tget(tb)) {
            (Ty::StrLit(x), Ty::StrLit(y)) => return if x == y { "true".into() } else { "false".into() },
            (Ty::StrLit(_), Ty::Str) | (Ty::Str, Ty::StrLit(_)) => {
                let a = self.coerce(a, STR);
                let b = self.coerce(b, STR);
                return format!("bm_str_eq({}, {})", a.code, b.code);
            }
            _ => {}
        }
        // A union compared with one of its members (e.g. `x === undefined`, `s.kind === "circle"`).
        for (u, x) in [(&a, &b), (&b, &a)] {
            if let Ty::Union(_) = self.tget(u.ty) {
                let target = self.member_target(u.ty, x.ty);
                if let Some(mt) = target {
                    let k = self.tag_of(u.ty, mt).unwrap();
                    if self.is_unit(mt) {
                        return self.u_is(&u.code, u.ty, k);
                    }
                    let xv = self.coerce(x.clone(), mt);
                    let inner = self.eq_code(mt, &self.u_payload(&u.code, u.ty, k), &xv.code);
                    return format!("({} && {inner})", self.u_is(&u.code, u.ty, k));
                }
                if let Ty::Str = self.tget(x.ty) {
                    // Union of literals compared with a string: convert the union.
                    let us = self.coerce(u.clone(), STR);
                    return format!("bm_str_eq({}, {})", us.code, x.code);
                }
            }
        }
        // Different unions: convert one to the other if possible.
        if self.c.assignable_pub(ta, tb) {
            let a2 = self.coerce(a, tb);
            return self.eq_code(tb, &a2.code, &b.code);
        }
        if self.c.assignable_pub(tb, ta) {
            let b2 = self.coerce(b, ta);
            return self.eq_code(ta, &a.code, &b2.code);
        }
        "false".into()
    }

    // ------------------------------------------------------------ members and indexing

    fn member(&mut self, e: ExprId, obj: ExprId, name: crate::intern::Sym, optional: bool, ty: TyId, span: Span) -> Val {
        let m = self.cur_m();
        match self.facts(m).members.get(&e).copied() {
            Some(MemberFact::NsFn(fm, fi)) => {
                let code = self.fn_value(fm, fi);
                return Val::plain(code, ty);
            }
            Some(MemberFact::NsConst(cm, ci)) => return self.const_ref(cm, ci),
            Some(MemberFact::StaticField(c, mi)) => return self.static_field_ref(c, mi),
            // `req.params` in a route handler: the record of the route's params, from the map.
            Some(MemberFact::RouteParams(rt)) => {
                let base = self.expr(obj);
                let map_ty = self.c.types.intern(Ty::Map(STR, STR));
                let mv = self.field(base, name, map_ty, span);
                let res = self.fresh("rp");
                let ct = self.ctype(rt);
                self.line(format!("{ct} {res};"));
                let Ty::Record(fs) = self.tget(rt) else { return Val::plain("0", ty) };
                for f in self.c.types.fields(fs).to_vec() {
                    let fname = self.sym(f.name).to_string();
                    let key = self.lit(fname.as_bytes());
                    let (kn, pn) = (self.fresh("k"), self.fresh("p"));
                    self.line(format!("bm_str {kn} = {key}; bm_str *{pn} = bm_map_get({}, &bm_type_str, &bm_type_str, &{kn});", mv.code));
                    self.line(format!("{res}.f_{fname} = {pn} ? *{pn} : BM_EMPTY_STR; bm_str_retain({res}.f_{fname});"));
                }
                self.b().temps.last_mut().unwrap().push((res.clone(), rt));
                return self.coerce(Val { code: res, ty: rt, owned: true }, ty);
            }
            Some(MemberFact::Env(var)) => {
                let lit = self.lit(self.sym(var).to_string().as_bytes());
                let (ok, sv) = (self.fresh("ok"), self.fresh("ev"));
                self.line(format!("bm_str {sv} = BM_EMPTY_STR; bool {ok} = bm_process_env({lit}, &{sv});"));
                self.b().temps.last_mut().unwrap().push((sv.clone(), STR));
                let v = Val { code: sv, ty: STR, owned: false };
                return self.optional_from_pub(&ok, v, ty);
            }
            Some(MemberFact::MathConst) if matches!(&self.ast(m).expr(obj).kind, ExprKind::Ident(s) if self.sym(*s) == "process") => {
                return match self.sym(name) {
                    "argv" => self.tmp(ty, "bm_process_argv()", true),
                    "platform" => self.tmp(STR, "bm_str_from(BMG_PLATFORM, sizeof(BMG_PLATFORM) - 1)", true),
                    _ => Val::plain("0", ty),
                };
            }
            Some(MemberFact::MathConst) => {
                let v = match self.sym(name) {
                    "PI" => "3.141592653589793",
                    "E" => "2.718281828459045",
                    "LN2" => "0.6931471805599453",
                    "LN10" => "2.302585092994046",
                    "LOG2E" => "1.4426950408889634",
                    "LOG10E" => "0.4342944819032518",
                    "SQRT2" => "1.4142135623730951",
                    _ => "0.7071067811865476",
                };
                return Val::plain(v, F64);
            }
            None => {}
        }
        let base = self.expr(obj);
        if optional {
            // a?.b: undefined when a is undefined.
            let res = self.fresh("t");
            let ct = self.ctype(ty);
            self.line(format!("{ct} {res};"));
            let present = self.truthy(base.clone());
            self.open(&format!("if ({present}) {{"));
            self.push_temps();
            let inner_ty = self.c.types.without_undefined(base.ty);
            let pb = self.project(base, inner_ty);
            // Read at the expression's type (which includes `undefined` unless narrowed):
            // an optional field keeps its own presence tag.
            let fv = self.field(pb, name, ty, span);
            let fv = self.coerce(fv, ty);
            let code = self.consume(fv);
            self.line(format!("{res} = {code};"));
            self.pop_temps();
            self.close("} else {");
            self.b().indent += 1;
            let undef = self.coerce(Val::plain("0", UNDEFINED), ty);
            let code = self.consume(undef);
            self.line(format!("{res} = {code};"));
            self.close("}");
            let owned = self.is_rc(ty);
            if owned {
                self.b().temps.last_mut().unwrap().push((res.clone(), ty));
            }
            return Val { code: res, ty, owned };
        }
        self.field(base, name, ty, span)
    }

    /// `base.name` for a non-optional base.
    fn field(&mut self, base: Val, name: crate::intern::Sym, ty: TyId, span: Span) -> Val {
        let base = self.unfold_val(base);
        let n = self.sym(name).to_string();
        match self.tget(base.ty) {
            Ty::Class(..) => self.class_field_read(base, name, ty, span),
            Ty::Interface(..) => match self.iface_field(base, name, ty) {
                Some(v) => v,
                None => {
                    self.unsupported(span, "using an interface method as a value");
                    Val::plain("0", ty)
                }
            },
            Ty::Record(fs) => {
                let f = self.c.types.fields(fs).iter().find(|f| f.name == name).copied();
                let Some(f) = f else {
                    self.unsupported(span, "this field access");
                    return Val::plain("0", ty);
                };
                let v = Val { code: format!("({}).f_{n}", base.code), ty: f.ty, owned: false };
                // The checker may have narrowed this field path (`if (t.left !== undefined) t.left...`).
                let v = if self.c.assignable_pub(ty, f.ty) && ty != f.ty && !self.c.assignable_pub(f.ty, ty) { self.project(v, ty) } else { self.coerce(v, ty) };
                if base.owned && !v.owned && self.is_rc(v.ty) {
                    // Borrowing from a temporary that will be released: keep an owned copy.
                    return self.own(v);
                }
                v
            }
            Ty::Union(ms) => {
                // Every member has the field: select by tag.
                let ms = self.c.types.tys(ms).to_vec();
                let res = self.fresh("f");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                let tag = self.u_tag(&base.code, base.ty);
                self.open(&format!("switch ({tag}) {{"));
                for (i, &mt) in ms.iter().enumerate() {
                    self.open(&format!("case {i}: {{"));
                    self.push_temps();
                    let payload = Val { code: self.u_payload(&base.code, base.ty, i), ty: mt, owned: false };
                    let fv = self.field(payload, name, ty, span);
                    let fv = self.coerce(fv, ty);
                    let code = self.consume(fv);
                    self.line(format!("{res} = {code};"));
                    self.pop_temps();
                    self.line("break;");
                    self.close("}");
                }
                let dv = self.default_value(ty);
                self.line(format!("default: {res} = {dv}; break;"));
                self.close("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            Ty::Array(_) if n == "length" => Val::plain(format!("bm_arr_len({})", base.code), INT),
            Ty::Str | Ty::StrLit(_) if n == "byteLength" => {
                let s = self.coerce(base, STR);
                Val::plain(format!("bm_str_byte_len({})", s.code), INT)
            }
            Ty::Map(..) | Ty::Set(_) if n == "size" => Val::plain(format!("bm_map_size({})", base.code), INT),
            _ => {
                self.unsupported(span, "this member access");
                Val::plain("0", ty)
            }
        }
    }

    fn index(&mut self, obj: ExprId, index: ExprId, optional: bool, ty: TyId, span: Span) -> Val {
        let _ = (optional, span);
        let a = self.expr(obj);
        let a = if optional {
            let inner = self.c.types.without_undefined(a.ty);
            if inner != a.ty {
                self.unsupported(span, "`?.[]` on an optional array");
            }
            self.project(a, inner)
        } else {
            a
        };
        let Ty::Array(et) = self.tget(a.ty) else {
            self.unsupported(span, "indexing this type");
            return Val::plain("0", ty);
        };
        let i = self.expr(index);
        let i = self.int_code(i);
        let iv = self.fresh("ix");
        self.line(format!("bm_int {iv} = {i};"));
        let res = self.fresh("t");
        let ct = self.ctype(ty);
        let ect = self.ctype(et);
        self.line(format!("{ct} {res};"));
        self.open(&format!("if ({iv} >= 0 && {iv} < bm_arr_len({})) {{", a.code));
        self.push_temps();
        let ev = Val::plain(format!("(({ect} *)bm_arr_data({}))[{iv}]", a.code), et);
        let cv = self.coerce(ev, ty);
        let code = self.consume(cv);
        self.line(format!("{res} = {code};"));
        self.pop_temps();
        self.close("} else {");
        self.b().indent += 1;
        let u = self.coerce(Val::plain("0", UNDEFINED), ty);
        let code = self.consume(u);
        self.line(format!("{res} = {code};"));
        self.close("}");
        let owned = self.is_rc(ty);
        if owned {
            self.b().temps.last_mut().unwrap().push((res.clone(), ty));
        }
        Val { code: res, ty, owned }
    }

    // ------------------------------------------------------------ places and assignment

    /// An lvalue for an assignment target or a mutated receiver (makes containers unique on the way).
    pub(crate) fn place(&mut self, e: ExprId) -> Place {
        let m = self.cur_m();
        let ast = self.ast(m);
        let span = ast.expr(e).span;
        match &ast.expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) => self.place(*x),
            ExprKind::Ident(_) => match self.ident_fact(m, e) {
                Some(IdentFact::Local(k)) => match self.b().locals.get(&k).cloned() {
                    Some(l) => Place { lv: l.access, ty: l.ty, weak: false },
                    None => {
                        self.unsupported(span, "assigning to this variable");
                        Place { lv: "bm__dummy".into(), ty: ERROR, weak: false }
                    }
                },
                Some(IdentFact::Const(cm, ci)) => {
                    let g = self.const_ref(cm, ci);
                    Place { lv: g.code, ty: g.ty, weak: false }
                }
                _ => {
                    self.unsupported(span, "assigning to this name");
                    Place { lv: "bm__dummy".into(), ty: ERROR, weak: false }
                }
            },
            ExprKind::Member { obj, name, .. } => {
                let narrowed = self.ty(*obj);
                let nt = self.c.types.without_undefined(narrowed);
                if self.c.class_of(nt).is_some() {
                    return self.class_field_place(*obj, *name, span);
                }
                let p = self.place(*obj);
                let p = self.project_place(p, narrowed);
                // Through a recursive alias: unbox (copy-on-write) to reach the record.
                let p = if matches!(self.tget(p.ty), Ty::Rec(..)) {
                    let inner = self.c.unfold(p.ty);
                    self.project_place(p, inner)
                } else {
                    p
                };
                let n = self.sym(*name).to_string();
                let ft = match self.tget(p.ty) {
                    Ty::Record(fs) => self.c.types.fields(fs).iter().find(|f| f.name == *name).map(|f| f.ty),
                    _ => None,
                };
                match ft {
                    Some(ft) => Place { lv: format!("({}).f_{n}", p.lv), ty: ft, weak: false },
                    None => {
                        self.unsupported(span, "assigning to this member");
                        Place { lv: "bm__dummy".into(), ty: ERROR, weak: false }
                    }
                }
            }
            ExprKind::Index { obj, index, .. } => {
                let p = self.place(*obj);
                let narrowed = self.ty(*obj);
                let p = self.project_place(p, narrowed);
                let Ty::Array(et) = self.tget(p.ty) else {
                    self.unsupported(span, "assigning through this index");
                    return Place { lv: "bm__dummy".into(), ty: ERROR, weak: false };
                };
                let i = self.expr(*index);
                let i = self.int_code(i);
                let d = self.desc(et);
                let ect = self.ctype(et);
                let loc = self.loc(span);
                let pv = self.fresh("ep");
                let unique = match (&ast.expr(*obj).kind, self.ident_fact(m, *obj)) {
                    (ExprKind::Ident(_), Some(IdentFact::Local(k))) => self.b().unique.contains(&k),
                    _ => false,
                };
                if unique {
                    self.line(format!("{ect} *{pv} = ({ect} *)bm_arr_at({}, sizeof({ect}), {i}, {loc});", p.lv));
                } else {
                    self.line(format!("{ect} *{pv} = ({ect} *)bmg_at_mut(&{}, {d}, sizeof({ect}), {i}, {loc});", p.lv));
                }
                Place { lv: format!("(*{pv})"), ty: et, weak: false }
            }
            _ => {
                // A temporary receiver (e.g. `xs.slice().sort()`): materialize it.
                let v = self.expr(e);
                let v = self.own(v);
                Place { lv: v.code, ty: v.ty, weak: false }
            }
        }
    }

    /// Narrows a place to a union member (payload lvalue), unboxing recursive aliases.
    fn project_place(&mut self, p: Place, to: TyId) -> Place {
        if p.ty == to || to == ERROR {
            return p;
        }
        if matches!(self.tget(p.ty), Ty::Rec(..)) {
            // Copy-on-write through the box.
            let inner = self.c.unfold(p.ty);
            let bn = self.box_name(p.ty);
            let bp = self.fresh("bp");
            let clone_inner = if self.is_rc(inner) { format!("{};", self.retain_code(inner, &format!("{bp}n->v"))) } else { String::new() };
            self.line(format!("{bn} **{bp} = &{};", p.lv));
            self.line(format!("if ((*{bp})->rc > 1) {{ {bn} *{bp}n = bmg_alloc_small(sizeof({bn})); {bp}n->rc = 1; {bp}n->v = (*{bp})->v; {clone_inner} (*{bp})->rc--; *{bp} = {bp}n; }}"));
            return self.project_place(Place { lv: format!("(*{bp})->v"), ty: inner, weak: false }, to);
        }
        if let Some(k) = self.tag_of(p.ty, to) {
            return Place { lv: self.u_payload(&p.lv, p.ty, k), ty: to, weak: false };
        }
        p
    }

    fn assign(&mut self, op: AssignOp, target: ExprId, value: ExprId, span: Span) -> Val {
        match op {
            AssignOp::Assign => {
                let v = self.expr(value);
                let p = self.place(target);
                if p.weak {
                    return self.weak_store(&p.lv, p.ty, v);
                }
                let v = self.coerce(v, p.ty);
                let code = self.consume(v);
                let ct = self.ctype(p.ty);
                let pv = self.fresh("p");
                self.line(format!("{ct} *{pv} = &{};", p.lv));
                if self.is_rc(p.ty) {
                    let nv = self.fresh("n");
                    self.line(format!("{ct} {nv} = {code};"));
                    let r = self.release_code(p.ty, &format!("(*{pv})"));
                    self.line(format!("{r}; *{pv} = {nv};"));
                } else {
                    self.line(format!("*{pv} = {code};"));
                }
                Val::plain(format!("(*{pv})"), p.ty)
            }
            AssignOp::Op(bop) => {
                let p = self.place(target);
                let ct = self.ctype(p.ty);
                let pv = self.fresh("p");
                self.line(format!("{ct} *{pv} = &{};", p.lv));
                let cur = Val::plain(format!("(*{pv})"), p.ty);
                let rhs = self.expr(value);
                let result = match bop {
                    BinOp::Nullish => {
                        let present = self.truthy(cur.clone());
                        self.open(&format!("if (!{present}) {{"));
                        self.push_temps();
                        let v = self.coerce(rhs, p.ty);
                        let code = self.consume(v);
                        let r = self.release_code(p.ty, &format!("(*{pv})"));
                        self.line(format!("{r}; *{pv} = {code};"));
                        self.pop_temps();
                        self.close("}");
                        return cur;
                    }
                    BinOp::Add if p.ty == STR => {
                        let sbv = self.fresh("sb");
                        self.line(format!("bm_sb {sbv} = {{0}};"));
                        self.open("{");
                        self.line(format!("bm_sb *sb = &{sbv};"));
                        self.line(format!("bm_sb_push_str(sb, *{pv});"));
                        let c = self.string_code(rhs.ty, &rhs.code);
                        self.line(format!("{c};"));
                        self.close("}");
                        let s = self.fresh("s");
                        self.line(format!("bm_str {s} = bm_str_from_sb(&{sbv});"));
                        self.line(format!("bm_str_release(*{pv}); *{pv} = {s};"));
                        return cur;
                    }
                    BinOp::BitAnd | BinOp::BitOr | BinOp::BitXor | BinOp::Shl | BinOp::Shr | BinOp::UShr => {
                        let o = match bop {
                            BinOp::BitAnd => "&",
                            BinOp::BitOr => "|",
                            BinOp::BitXor => "^",
                            BinOp::Shl => "<<",
                            _ => ">>",
                        };
                        Val::plain(format!("(({ct})({} {o} {}))", cur.code, rhs.code), p.ty)
                    }
                    _ => {
                        let arith_ty = if bop == BinOp::Div || self.c.types.is_float(p.ty) || self.c.types.is_float(rhs.ty) {
                            if p.ty == F32 { F32 } else { F64 }
                        } else {
                            p.ty
                        };
                        let r = self.arith(bop, cur.clone(), rhs, arith_ty, span);
                        self.coerce(r, p.ty)
                    }
                };
                self.line(format!("*{pv} = {};", result.code));
                cur
            }
        }
    }

    // ------------------------------------------------------------ closures

    /// A `bm_fn` value for an arrow expression (environment on the heap).
    fn closure_value(&mut self, e: ExprId, fty: TyId) -> Val {
        let info = self.emit_arrow(e, fty, false);
        if info.captures.is_empty() {
            return Val::plain(format!("((bm_fn){{ (void *){}, NULL }})", info.cname), fty);
        }
        let env = self.fresh("env");
        let et = info.env_type.clone();
        self.line(format!("{et} *{env} = bm_alloc(sizeof({et})); {env}->h.rc = 1; {env}->h.drop = drop_{et};"));
        for (j, (key, ty, boxed)) in info.captures.iter().enumerate() {
            let local = self.b().locals.get(key).cloned();
            let Some(local) = local else { continue };
            if *boxed {
                // Share the cell.
                let cell = local.access.trim_end_matches("->v").to_string();
                self.line(format!("{env}->c{j} = {cell}; {cell}->rc++;"));
            } else {
                let v = Val::plain(local.access.clone(), *ty);
                let code = self.consume(v);
                self.line(format!("{env}->c{j} = {code};"));
            }
        }
        self.tmp(fty, &format!("((bm_fn){{ (void *){}, (bm_env *){env} }})", info.cname), true)
    }

    /// Emits the C function for an arrow. Stack mode: the environment holds pointers to the
    /// captured variables (valid for the duration of a built-in method call).
    pub(crate) fn emit_arrow(&mut self, e: ExprId, fty: TyId, stack: bool) -> ArrowInfo {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Arrow(f) = &ast.expr(e).kind else { unreachable!() };
        let Ty::Func(ps, ret, _) = self.tget(fty) else {
            let span = self.expr_span(e);
            self.unsupported(span, "this function value");
            return ArrowInfo { cname: "NULL".into(), env_type: String::new(), captures: Vec::new() };
        };
        let ps = self.c.types.params(ps).to_vec();
        let cname = self.fresh("arrow");
        let captures_keys = arrow_captures(ast, self.facts(m), e);
        let boxed = self.b().boxed.clone();
        let mut captures: Vec<(u32, TyId, bool)> = Vec::new();
        for k in captures_keys {
            if let Some(l) = self.b().locals.get(&k).cloned() {
                captures.push((k, l.ty, boxed.contains(&k) && !stack));
            }
        }
        let env_type = format!("E{}", &cname[5..]);
        if !captures.is_empty() {
            let mut fields = String::new();
            for (j, (_, ty, bx)) in captures.iter().enumerate() {
                let ct = self.ctype(*ty);
                if stack {
                    let _ = write!(fields, " {ct} *c{j};");
                } else if *bx {
                    let cell = self.cell_type(*ty);
                    let _ = write!(fields, " {cell} *c{j};");
                } else {
                    let _ = write!(fields, " {ct} c{j};");
                }
            }
            // Environments hold captured values by value: defined after every value struct.
            let _ = writeln!(self.typedefs, "typedef struct {env_type} {env_type};");
            let _ = writeln!(self.env_structs, "struct {env_type} {{ bm_env h;{fields} }};");
            if !stack {
                let mut drops = String::new();
                for (j, (_, ty, bx)) in captures.iter().enumerate() {
                    if *bx {
                        let _ = write!(drops, " {}", self.cell_release(*ty, &format!("x->c{j}")));
                    } else if self.is_rc(*ty) {
                        let _ = write!(drops, " {};", self.release_code(*ty, &format!("x->c{j}")));
                    }
                }
                let _ = writeln!(self.protos, "static void drop_{env_type}(bm_env *e);");
                let _ = writeln!(self.helpers, "static void drop_{env_type}(bm_env *e) {{ {env_type} *x = ({env_type} *)e; (void)x;{drops} }}");
            }
        }
        // Emit the function body with captured locals mapped into the environment.
        let subst = self.b().subst.clone();
        let params: Vec<(u32, TyId, bool)> = f.params.iter().zip(&ps).map(|(p, fp)| (p.span.start, fp.ty, fp.inout)).collect();
        let proto_params: Vec<FnParam> = ps.clone();
        let proto = self.fn_proto(&cname, &proto_params, ret, &FxMap::default(), true);
        let _ = writeln!(self.protos, "static {proto};");
        let kind = match &f.body {
            ArrowBody::Block(b) => FnBodyKind::Block(*b),
            ArrowBody::Expr(x) => FnBodyKind::Expr(*x),
        };
        let mut env_locals = FxMap::default();
        for (j, (k, ty, bx)) in captures.iter().enumerate() {
            let access = if stack {
                format!("(*(({env_type} *)env_)->c{j})")
            } else if *bx {
                format!("(({env_type} *)env_)->c{j}->v")
            } else {
                format!("(({env_type} *)env_)->c{j}")
            };
            env_locals.insert(*k, Local { access, ty: *ty });
        }
        let code = self.arrow_body(m, subst, &params, ret, kind, env_locals);
        let _ = writeln!(self.funcs, "static {proto} {{\n    (void)env_;\n{code}}}\n");
        ArrowInfo { cname, env_type, captures }
    }

    fn arrow_body(&mut self, m: u32, subst: FxMap<u32, TyId>, params: &[(u32, TyId, bool)], ret: TyId, kind: FnBodyKind, env_locals: FxMap<u32, Local>) -> String {
        // Captured variables are visible as locals; the enclosing function's boxing decisions carry over.
        let outer_boxed = self.b().boxed.clone();
        let outer_stack = self.b().stack_arrows.clone();
        let (inner_boxed, inner_stack, inner_mutated) = self.analyze_closures(m, &kind);
        let mut body = Body {
            m,
            subst,
            out: String::new(),
            indent: 1,
            locals: env_locals,
            scopes: vec![Scope { releases: Vec::new() }],
            temps: vec![Vec::new()],
            ret,
            loops: Vec::new(),
            breaks: Vec::new(),
            boxed: outer_boxed.union(&inner_boxed).copied().collect(),
            stack_arrows: outer_stack.union(&inner_stack).copied().collect(),
            unique: FxSet::default(),
            mutated: inner_mutated,
            params: params.iter().filter(|p| !p.2).map(|p| p.0).collect(),
            ctor_class: None,
            handlers: Vec::new(),
            finallies: Vec::new(),
        };
        for (i, &(key, ty, inout)) in params.iter().enumerate() {
            let access = if inout { format!("(*p{i})") } else { format!("p{i}") };
            body.locals.insert(key, Local { access, ty });
        }
        self.bodies.push(body);
        match kind {
            FnBodyKind::Block(s) => {
                self.stmt_list_of(s);
                self.unwind_to(0);
                if ret != VOID && !self.always_returns_stmt(m, s) {
                    let v = self.coerce(Val::plain("0", UNDEFINED), ret);
                    let code = self.consume(v);
                    self.line(format!("return {code};"));
                }
            }
            FnBodyKind::Expr(e) => {
                let v = self.expr(e);
                if ret == VOID {
                    self.pop_temps();
                    self.b().temps.push(Vec::new());
                    self.unwind_to(0);
                } else {
                    let v = self.coerce(v, ret);
                    let code = self.consume(v);
                    let ct = self.ctype(ret);
                    self.line(format!("{ct} r_ = {code};"));
                    self.release_all_temps();
                    self.unwind_to(0);
                    self.line("return r_;");
                }
            }
            FnBodyKind::Init => {}
        }
        self.bodies.pop().unwrap().out
    }
}

pub(crate) struct Place {
    pub lv: String,
    pub ty: TyId,
    /// A weak class field (weak counts instead of strong ones).
    pub weak: bool,
}

pub(crate) struct ArrowInfo {
    pub cname: String,
    pub env_type: String,
    /// (declaration key, type, boxed)
    pub captures: Vec<(u32, TyId, bool)>,
}

/// A C expression with no side effects worth keeping (a name, a literal, a field path).
fn is_trivial(code: &str) -> bool {
    code.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '(' | ')' | '*' | '-' | '>' | ' '))
        && !code.contains("->") && !code.contains('(')
        || code == "0"
}

pub(crate) fn is_mutating_method(name: &str) -> bool {
    matches!(name, "push" | "pop" | "shift" | "unshift" | "reverse" | "sort" | "set" | "delete" | "clear" | "add")
}

/// Locals referenced inside an arrow (including nested arrows) but declared outside it.
fn arrow_captures(ast: &ast::Ast, facts: &crate::check::ModuleFacts, arrow: ExprId) -> Vec<u32> {
    let span = ast.expr(arrow).span;
    let mut exprs = Vec::new();
    collect_exprs_expr(ast, arrow, &mut exprs);
    let mut out: Vec<u32> = Vec::new();
    for e in exprs {
        if let ExprKind::Ident(_) | ExprKind::This = ast.expr(e).kind
            && let Some(IdentFact::Local(k)) = facts.idents.get(&e)
            && (*k < span.start || *k >= span.end)
            && !out.contains(k)
        {
            out.push(*k);
        }
    }
    out
}

pub(crate) fn collect_exprs_stmt_pub(ast: &ast::Ast, s: StmtId, out: &mut Vec<ExprId>) {
    collect_exprs_stmt(ast, s, out)
}

fn collect_exprs_stmt(ast: &ast::Ast, s: StmtId, out: &mut Vec<ExprId>) {
    match &ast.stmt(s).kind {
        StmtKind::Expr(e) => collect_exprs_expr(ast, *e, out),
        StmtKind::Let { init: Some(e), .. } => collect_exprs_expr(ast, *e, out),
        StmtKind::If(c, t, e) => {
            collect_exprs_expr(ast, *c, out);
            collect_exprs_stmt(ast, *t, out);
            if let Some(e) = e {
                collect_exprs_stmt(ast, *e, out);
            }
        }
        StmtKind::While(c, b) | StmtKind::DoWhile(b, c) => {
            collect_exprs_expr(ast, *c, out);
            collect_exprs_stmt(ast, *b, out);
        }
        StmtKind::For { init, cond, step, body } => {
            if let Some(i) = init {
                collect_exprs_stmt(ast, *i, out);
            }
            for e in [cond, step].into_iter().flatten() {
                collect_exprs_expr(ast, *e, out);
            }
            collect_exprs_stmt(ast, *body, out);
        }
        StmtKind::ForOf { iter, body, .. } => {
            collect_exprs_expr(ast, *iter, out);
            collect_exprs_stmt(ast, *body, out);
        }
        StmtKind::Switch(d, cases) => {
            collect_exprs_expr(ast, *d, out);
            for c in cases {
                if let Some(t) = c.test {
                    collect_exprs_expr(ast, t, out);
                }
                for &x in &c.body {
                    collect_exprs_stmt(ast, x, out);
                }
            }
        }
        StmtKind::Return(Some(e)) | StmtKind::Throw(e) => collect_exprs_expr(ast, *e, out),
        StmtKind::Block(ss) => ss.iter().for_each(|&x| collect_exprs_stmt(ast, x, out)),
        StmtKind::Try { body, catch, finally } => {
            collect_exprs_stmt(ast, *body, out);
            if let Some(c) = catch {
                collect_exprs_stmt(ast, c.body, out);
            }
            if let Some(f) = finally {
                collect_exprs_stmt(ast, *f, out);
            }
        }
        _ => {}
    }
}

fn collect_exprs_expr(ast: &ast::Ast, e: ExprId, out: &mut Vec<ExprId>) {
    out.push(e);
    match &ast.expr(e).kind {
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::As(x, _) | ExprKind::Try(x) | ExprKind::Await(x) => collect_exprs_expr(ast, *x, out),
        ExprKind::Binary(_, l, r) | ExprKind::Assign(_, l, r) => {
            collect_exprs_expr(ast, *l, out);
            collect_exprs_expr(ast, *r, out);
        }
        ExprKind::Update { target, .. } => collect_exprs_expr(ast, *target, out),
        ExprKind::Call { callee, args, .. } | ExprKind::New { callee, args, .. } => {
            collect_exprs_expr(ast, *callee, out);
            for a in args {
                collect_exprs_expr(ast, a.expr, out);
            }
        }
        ExprKind::Member { obj, .. } => collect_exprs_expr(ast, *obj, out),
        ExprKind::Index { obj, index, .. } => {
            collect_exprs_expr(ast, *obj, out);
            collect_exprs_expr(ast, *index, out);
        }
        ExprKind::Cond(a, b, c) => {
            for x in [a, b, c] {
                collect_exprs_expr(ast, *x, out);
            }
        }
        ExprKind::Array(xs) | ExprKind::Template(_, xs) => xs.iter().for_each(|&x| collect_exprs_expr(ast, x, out)),
        ExprKind::Object(fs) => fs.iter().for_each(|f| collect_exprs_expr(ast, f.value, out)),
        ExprKind::Arrow(f) => match &f.body {
            ArrowBody::Expr(x) => collect_exprs_expr(ast, *x, out),
            ArrowBody::Block(b) => collect_exprs_stmt(ast, *b, out),
        },
        _ => {}
    }
}
