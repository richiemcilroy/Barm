//! Async functions (real async, `BARM_ASYNC`). An async function `f` compiles to:
//! - a frame `AF_f`: `pc` (the label to resume at), `ret`, the parameters and, after
//!   `frame::lower`, every local and temporary, plus a union `u` of the frames of the async calls
//!   it awaits directly (only one is in progress at a time);
//! - `f(AF_f *F)`: runs from `F->pc`, returns true once finished (the result in `F->ret`, or an
//!   error in `bmg_err`) and false when it waits, after arranging to be resumed;
//! - `f_task` / `f_spawn`: runs it as a task of its own, for calls that aren't awaited (the task's
//!   promise is the call's value).
//! A task resumes from its root frame, and every frame re-enters its child at the pending
//! `await`, so no frame needs a pointer to its parent and none ever moves.

use super::{Gen, Instance, Val};
use crate::ast::{self, ExprId, ExprKind};
use crate::check::Callee;
use crate::hash::{FxMap, FxSet};
use crate::types::*;
use std::fmt::Write;

/// A frame struct: (name, frames it embeds, definition).
pub(crate) struct FrameDef {
    pub name: String,
    pub deps: Vec<String>,
    pub text: String,
}

impl<'c, 'a> Gen<'c, 'a> {
    /// Is (module, item) an async function (with real async on)?
    pub(crate) fn is_async_fn(&self, m: u32, item: u32) -> bool {
        crate::async_enabled() && matches!(&self.ast(m).items[item as usize].kind, ast::ItemKind::Function(f) if f.is_async)
    }

    /// The (instantiated) type an async function's promise resolves to.
    pub(crate) fn async_value_type(&mut self, ret: TyId) -> TyId {
        match self.tget(ret) {
            Ty::Promise(v, _) => v,
            _ => ret,
        }
    }

    pub(super) fn emit_async_function(&mut self, inst: Instance) {
        let ast::ItemKind::Function(f) = &self.ast(inst.m).items[inst.item as usize].kind else { return };
        let sig = self.c.sigs[&(inst.m, inst.item)].clone();
        let cname = inst.cname.clone();
        let frame = format!("AF_{cname}");
        let params: Vec<(u32, TyId, bool)> = f.params.iter().zip(&sig.params).map(|(p, fp)| (p.span.start, self.c.types.subst(fp.ty, &inst.subst), false)).collect();
        let ret_p = self.c.types.subst(sig.ret, &inst.subst);
        let ret = self.async_value_type(ret_p);
        self.pending_async = Some((inst.m, inst.item));
        let code = self.function_body(inst.m, inst.subst.clone(), &params, ret, super::body::FnBodyKind::Block(f.body), None, None, None);
        let ptys: Vec<TyId> = params.iter().map(|p| p.1).collect();
        if !self.async_parts(&cname, &format!("{cname}_task"), &frame, &code, &ptys, ret, false) {
            return;
        }
        // Started as a task of its own (a call that isn't awaited).
        let proto = self.spawn_proto(&cname, &params);
        let mut set = String::new();
        for (i, ty) in ptys.iter().enumerate() {
            let r = if self.is_rc(*ty) { format!(" {};", self.retain_code(*ty, &format!("p{i}"))) } else { String::new() };
            let _ = write!(set, " F->p{i} = p{i};{r}");
        }
        let desc = self.desc(ret);
        let _ = writeln!(
            self.funcs,
            "static {proto} {{\n    bm_task *t = bm_task_new(sizeof({frame}), {cname}_task, {desc});\n    {frame} *F = bm_task_frame(t);{set}\n    return bm_task_spawn(t);\n}}\n"
        );
    }

    /// An async arrow: its closure function starts a task (holding the environment) and returns
    /// the task's promise.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn emit_async_arrow(&mut self, cname: &str, m: u32, subst: FxMap<u32, TyId>, params: &[(u32, TyId, bool)], ret_p: TyId, kind: super::body::FnBodyKind, env_locals: FxMap<u32, super::body::Local>) {
        let ret = self.async_value_type(ret_p);
        let frame = format!("AF_{cname}");
        let _ = writeln!(self.typedefs, "typedef struct {frame} {frame};");
        self.pending_async = Some((u32::MAX, self.counter));
        let code = self.arrow_body_pub(m, subst, params, ret, kind, env_locals);
        let ptys: Vec<TyId> = params.iter().map(|p| p.1).collect();
        let resume = format!("{cname}_resume");
        let _ = writeln!(self.protos, "static bool {resume}({frame} *F);\nstatic bool {cname}_task(bm_task *t);");
        if !self.async_parts(&resume, &format!("{cname}_task"), &frame, &code, &ptys, ret, true) {
            return;
        }
        let mut ps = vec!["bm_env *env_".to_string()];
        let mut set = String::from(" F->env_ = env_; bm_env_retain(env_);");
        for (i, ty) in ptys.iter().enumerate() {
            ps.push(format!("{} p{i}", self.ctype(*ty)));
            let r = if self.is_rc(*ty) { format!(" {};", self.retain_code(*ty, &format!("p{i}"))) } else { String::new() };
            let _ = write!(set, " F->p{i} = p{i};{r}");
        }
        let desc = self.desc(ret);
        let _ = writeln!(
            self.funcs,
            "static bm_promise *{cname}({}) {{\n    bm_task *t = bm_task_new(sizeof({frame}), {cname}_task, {desc});\n    {frame} *F = bm_task_frame(t);{set}\n    return bm_task_spawn(t);\n}}\n",
            ps.join(", ")
        );
    }

    /// The frame, resume function and task function of an async body; false after an internal
    /// error. `env`: an async arrow's frame also holds its closure environment (`env_`).
    #[allow(clippy::too_many_arguments)]
    fn async_parts(&mut self, resume: &str, task: &str, frame: &str, code: &str, params: &[TyId], ret: TyId, env: bool) -> bool {
        let children = std::mem::take(&mut self.last_children);
        let mut names: Vec<String> = (0..params.len()).map(|i| format!("p{i}")).collect();
        if env {
            names.push("env_".into());
        }
        let lowered = match super::frame::lower(code, &names) {
            Ok(l) => l,
            Err(msg) => {
                self.internal.push(format!("async function `{resume}`: {msg}"));
                return false;
            }
        };
        let mut def = format!("struct {frame} {{\n    void *pc;\n");
        if ret != VOID {
            let ct = self.ctype(ret);
            let _ = writeln!(def, "    {ct} ret;");
        }
        if env {
            def.push_str("    bm_env *env_;\n");
        }
        for (i, ty) in params.iter().enumerate() {
            let ct = self.ctype(*ty);
            let _ = writeln!(def, "    {ct} p{i};");
        }
        for fl in &lowered.fields {
            let _ = writeln!(def, "    {} {};", fl.ty, fl.name);
        }
        let mut deps = Vec::new();
        if !children.is_empty() {
            def.push_str("    union {\n");
            for (member, ty) in &children {
                let _ = writeln!(def, "        {ty} {member};");
                deps.push(ty.clone());
            }
            def.push_str("    } u;\n");
        }
        def.push_str("};\n");
        self.frame_defs.push(FrameDef { name: frame.to_string(), deps, text: def });
        // (A body without `await` never suspends: it has nowhere to resume.)
        let dispatch = if lowered.body.contains("&&aw") { "    if (F->pc) goto *F->pc;\n" } else { "    (void)F;\n" };
        let _ = writeln!(self.funcs, "static bool {resume}({frame} *F) {{\n{dispatch}{}    return true;\n}}\n", lowered.body);
        // Run as a task: settle the task's promise, then release what the frame was given.
        let mut release = String::new();
        for (i, ty) in params.iter().enumerate() {
            if self.is_rc(*ty) {
                let r = self.release_code(*ty, &format!("F->p{i}"));
                let _ = write!(release, " {r};");
            }
        }
        if env {
            release.push_str(" bm_env_release(F->env_);");
        }
        let settle = if ret == VOID { "bm_promise_resolve_move(t->promise, NULL);".to_string() } else { "bm_promise_resolve_move(t->promise, &F->ret);".to_string() };
        let _ = writeln!(
            self.funcs,
            "static bool {task}(bm_task *t) {{\n    {frame} *F = bm_task_frame(t);\n    if (!{resume}(F)) return false;\n    if (bmg_err) {{ bm_promise_reject(t->promise, bmg_err); bmg_err = NULL; }} else {settle}\n   {release}\n    return true;\n}}\n"
        );
        true
    }

    /// `bm_promise *f_spawn(params)`.
    fn spawn_proto(&mut self, cname: &str, params: &[(u32, TyId, bool)]) -> String {
        let ps: Vec<String> = params.iter().enumerate().map(|(i, (_, ty, _))| format!("{} p{i}", self.ctype(*ty))).collect();
        let ps = if ps.is_empty() { "void".to_string() } else { ps.join(", ") };
        format!("bm_promise *{cname}_spawn({ps})")
    }

    /// Declarations for an async function instance (its frame type, resume and spawn functions).
    pub(crate) fn declare_async(&mut self, cname: &str, sig_params: &[FnParam], subst: &FxMap<u32, TyId>) {
        let frame = format!("AF_{cname}");
        let _ = writeln!(self.typedefs, "typedef struct {frame} {frame};");
        let params: Vec<(u32, TyId, bool)> = sig_params.iter().map(|p| (0, self.c.types.subst(p.ty, subst), false)).collect();
        let proto = self.spawn_proto(cname, &params);
        let _ = writeln!(self.protos, "static bool {cname}({frame} *F);\nstatic bool {cname}_task(bm_task *t);\nstatic {proto};");
    }

    /// Frame structs in an order C accepts: embedded frames before the frames embedding them.
    pub(crate) fn ordered_frames(&self) -> String {
        let mut out = String::new();
        let mut done: FxSet<&str> = FxSet::default();
        fn visit<'x>(name: &'x str, defs: &'x [FrameDef], done: &mut FxSet<&'x str>, out: &mut String) {
            if !done.insert(name) {
                return;
            }
            let Some(d) = defs.iter().find(|d| d.name == name) else { return };
            for dep in &d.deps {
                visit(dep, defs, done, out);
            }
            out.push_str(&d.text);
        }
        for d in &self.frame_defs {
            visit(&d.name, &self.frame_defs, &mut done, &mut out);
        }
        out
    }

    /// Does awaiting (module, item) lead back to `target` through direct awaits? Then its frame
    /// can't be embedded (it would contain itself).
    fn awaits_reach(&self, from: (u32, u32), target: (u32, u32), seen: &mut FxSet<(u32, u32)>) -> bool {
        if from == target {
            return true;
        }
        if !seen.insert(from) {
            return false;
        }
        let (m, item) = from;
        let ast::ItemKind::Function(f) = &self.ast(m).items[item as usize].kind else { return false };
        let mut exprs = Vec::new();
        super::body::collect_exprs_stmt(self.ast(m), f.body, &mut exprs);
        for e in exprs {
            let ExprKind::Await(x) = self.ast(m).expr(e).kind else { continue };
            if let Some((cm, ci)) = self.direct_async_call(m, x)
                && self.awaits_reach((cm, ci), target, seen)
            {
                return true;
            }
        }
        false
    }

    /// `x` is a call to a module async function: (module, item).
    fn direct_async_call(&self, m: u32, mut x: ExprId) -> Option<(u32, u32)> {
        while let ExprKind::Paren(inner) = self.ast(m).expr(x).kind {
            x = inner;
        }
        let ExprKind::Call { .. } = self.ast(m).expr(x).kind else { return None };
        match self.facts(m).calls.get(&x)?.callee {
            Callee::Fn(fm, fi) if self.is_async_fn(fm, fi) => Some((fm, fi)),
            _ => None,
        }
    }

    /// `await x` inside an async function body.
    pub(crate) fn await_expr(&mut self, e: ExprId, x: ExprId) -> Val {
        let m = self.cur_m();
        let ty = self.ty(e);
        let Some(cur) = self.b().async_fn else {
            let span = self.expr_span(e);
            self.unsupported(span, "`await` here");
            return Val::plain("0", ty);
        };
        let n = self.fresh("");
        if let Some((cm, ci)) = self.direct_async_call(m, x)
            && !self.awaits_reach((cm, ci), cur, &mut FxSet::default())
        {
            return self.await_embedded(e, x, n);
        }
        let v = self.expr(x);
        let v = self.own(v);
        let value_ty = self.c.types.without_undefined(v.ty);
        let Ty::Promise(inner, _) = self.tget(value_ty) else {
            // `await value`: the value, one tick later.
            self.line(format!("if (!bm_async_eager()) {{ F->pc = &&aw{n}; bm_task_yield(); BM_SUSPEND; }}"));
            self.line(format!("aw{n}:;"));
            return self.coerce(v, ty);
        };
        if value_ty != v.ty {
            let span = self.expr_span(e);
            self.unsupported(span, "awaiting an optional promise");
            return Val::plain("0", ty);
        }
        let p = v.code.clone();
        self.line(format!("if (!bm_await_now({p})) {{ F->pc = &&aw{n}; bm_await_suspend({p}); BM_SUSPEND; }}"));
        self.line(format!("aw{n}:;"));
        self.take_settled(e, &p, inner, ty)
    }

    /// The value of a settled promise (a rejection sets `bmg_err`, checked when `e` can throw).
    fn take_settled(&mut self, e: ExprId, p: &str, inner: TyId, ty: TyId) -> Val {
        let m = self.cur_m();
        let throwing = self.facts(m).throwing.contains(&e);
        if inner == VOID || self.is_unit(inner) {
            if throwing {
                self.line(format!("if ({p}->state == BM_REJECTED) {{ bmg_err = {p}->err; bmg_obj_retain(bmg_err); }}"));
                self.error_check();
            }
            return Val::plain("0", ty);
        }
        let ct = self.ctype(inner);
        let d = self.default_value(inner);
        let res = self.tmp(inner, &d, true);
        let r = self.retain_code(inner, &res.code);
        self.line(format!("if ({p}->state == BM_FULFILLED) {{ {} = *({ct} *)bm_promise_value({p}); {r}; }} else {{ bmg_err = {p}->err; bmg_obj_retain(bmg_err); }}", res.code));
        if throwing {
            self.error_check();
        }
        self.coerce(res, ty)
    }

    /// `await f(args)` with `f` an async module function: `f`'s frame is embedded in this one.
    fn await_embedded(&mut self, e: ExprId, x: ExprId, n: String) -> Val {
        let m = self.cur_m();
        let ty = self.ty(e);
        let mut call = x;
        while let ExprKind::Paren(inner) = self.ast(m).expr(call).kind {
            call = inner;
        }
        let ExprKind::Call { args, .. } = &self.ast(m).expr(call).kind else { unreachable!() };
        let fact = self.facts(m).calls.get(&call).cloned().expect("call fact");
        let Callee::Fn(fm, fi) = fact.callee else { unreachable!() };
        let mut subst = FxMap::default();
        for (p, t) in &fact.targs {
            let t = self.inst(*t);
            subst.insert(*p, t);
        }
        let child = self.instance(fm, fi, subst);
        let params: Vec<FnParam> = fact.params.iter().map(|p| FnParam { ty: self.inst(p.ty), ..*p }).collect();
        // The arguments stay alive (owned by this statement) while the call may be suspended.
        let mut argv = Vec::new();
        for (i, a) in args.iter().enumerate() {
            let p = params.get(i).copied().unwrap_or(FnParam { ty: ERROR, inout: false, optional: false });
            let v = self.expr(a.expr);
            let v = self.coerce(v, p.ty);
            let v = self.own(v);
            argv.push(v.code);
        }
        for p in params.iter().skip(args.len()) {
            let v = self.coerce(Val::plain("0", UNDEFINED), p.ty);
            argv.push(v.code);
        }
        let member = format!("aw{n}");
        self.b().children.push((member.clone(), format!("AF_{child}")));
        let fr = format!("F->u.{member}");
        self.line(format!("memset(&{fr}, 0, sizeof {fr});"));
        for (i, a) in argv.iter().enumerate() {
            self.line(format!("{fr}.p{i} = {a};"));
        }
        // Resume here while the call is in progress; once it has finished, the promise it stands
        // for would take a tick to reach this `await` (its error waits in `e<n>` meanwhile).
        self.line(format!("F->pc = &&aw{n};"));
        self.line(format!("aw{n}:;"));
        self.line(format!("if (!{child}(&{fr})) BM_SUSPEND;"));
        let en = format!("e{n}");
        self.line(format!("void *{en} = NULL;"));
        self.line(format!("if (!bm_async_eager()) {{ {en} = bmg_err; bmg_err = NULL; F->pc = &&aw{n}t; bm_task_yield(); BM_SUSPEND; }}"));
        self.line(format!("if (0) {{ aw{n}t:; bmg_err = {en}; }}"));
        let ret = self.inst(fact.ret);
        let inner = self.async_value_type(ret);
        let throwing = self.facts(m).throwing.contains(&e);
        let v = if inner == VOID || self.is_unit(inner) {
            Val::plain("0", ty)
        } else {
            // The callee's result is owned (its error path leaves a valid default).
            let v = self.tmp(inner, &format!("{fr}.ret"), true);
            self.coerce(v, ty)
        };
        if throwing {
            self.error_check();
        }
        v
    }

    /// `new Promise((resolve, reject) => ...)`: the executor runs now with two closures that
    /// settle the promise (each holds a reference to it).
    pub(crate) fn new_promise(&mut self, args: &[ast::Arg], ty: TyId) -> Val {
        let Ty::Promise(value, _) = self.tget(ty) else { return Val::plain("NULL", ty) };
        let desc = self.desc(value);
        let p = self.tmp(ty, &format!("bm_promise_new({desc})"), true);
        let resolve = self.resolver_thunk(value);
        let Some(arg) = args.first() else { return p };
        let exec = self.expr(arg.expr);
        let (fv, rs, rj) = (self.fresh("fn"), self.fresh("rs"), self.fresh("rj"));
        self.line(format!("bm_fn {fv} = {};", exec.code));
        self.line(format!("bm_fn {rs} = {{ (void *){resolve}, bm_promise_resolver({}) }};", p.code));
        self.line(format!("bm_fn {rj} = {{ (void *)bmg_reject, bm_promise_resolver({}) }};", p.code));
        self.line(format!("((void (*)(bm_env *, bm_fn, bm_fn)){fv}.fn)({fv}.env, {rs}, {rj});"));
        self.line(format!("bm_env_release({rs}.env); bm_env_release({rj}.env);"));
        p
    }

    /// `resolve(value)` for promises of `value`'s type.
    fn resolver_thunk(&mut self, value: TyId) -> String {
        let name = format!("bmg_resolve_{}", value.0);
        if self.helpers_done.insert((value, super::H_RESOLVE)) {
            let ct = self.ctype(value);
            let _ = writeln!(self.protos, "static void {name}(bm_env *env, {ct} v);");
            let _ = writeln!(self.helpers, "static void {name}(bm_env *env, {ct} v) {{ bm_promise_resolve(bm_resolver_promise(env), &v); }}");
        }
        if self.helpers_done.insert((VOID, super::H_REJECT)) {
            let _ = writeln!(self.protos, "static void bmg_reject(bm_env *env, void *err);");
            let _ = writeln!(self.helpers, "static void bmg_reject(bm_env *env, void *err) {{ bmg_obj_retain(err); bm_promise_reject(bm_resolver_promise(env), err); }}");
        }
        name
    }

    /// Does evaluating `e` await (outside nested functions), in an async body?
    pub(crate) fn awaits_in(&self, e: ExprId) -> bool {
        self.bodies.last().is_some_and(|b| b.async_fn.is_some()) && has_await(self.ast(self.cur_m()), e)
    }

    /// Evaluates `e`; if one of `later` awaits, reads its value now (JavaScript evaluates operands
    /// left to right, so another task can't change what was already read).
    pub(crate) fn expr_before(&mut self, e: ExprId, later: &[ExprId]) -> Val {
        let v = self.expr(e);
        if later.iter().any(|&l| self.awaits_in(l)) { self.snapshot(v) } else { v }
    }

    /// The value now, in a temporary of its own.
    pub(crate) fn snapshot(&mut self, v: Val) -> Val {
        if v.owned {
            return v;
        }
        if self.is_rc(v.ty) { self.own(v) } else { self.tmp(v.ty, &v.code.clone(), false) }
    }

    /// A call to an async function that isn't awaited: it runs as a task; its promise is the value.
    pub(crate) fn spawn_call(&mut self, cname: &str, argv: &[String], ret: TyId, ty: TyId) -> Val {
        let v = self.tmp(ret, &format!("{cname}_spawn({})", argv.join(", ")), true);
        self.coerce(v, ty)
    }
}

/// Does `e` contain an `await` outside nested functions?
fn has_await(ast: &ast::Ast, e: ExprId) -> bool {
    let any = |xs: &[ExprId]| xs.iter().any(|&x| has_await(ast, x));
    match &ast.expr(e).kind {
        ExprKind::Await(_) => true,
        ExprKind::Arrow(_) => false,
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::As(x, _) | ExprKind::Try(x) => has_await(ast, *x),
        ExprKind::Update { target, .. } => has_await(ast, *target),
        ExprKind::Binary(_, a, b) | ExprKind::Assign(_, a, b) | ExprKind::Index { obj: a, index: b, .. } => has_await(ast, *a) || has_await(ast, *b),
        ExprKind::Cond(a, b, c) => has_await(ast, *a) || has_await(ast, *b) || has_await(ast, *c),
        ExprKind::Call { callee, args, .. } | ExprKind::New { callee, args, .. } => has_await(ast, *callee) || args.iter().any(|a| has_await(ast, a.expr)),
        ExprKind::Member { obj, .. } => has_await(ast, *obj),
        ExprKind::Template(_, xs) | ExprKind::Array(xs) => any(xs),
        ExprKind::Object(fs) => fs.iter().any(|f| has_await(ast, f.value)),
        ExprKind::Int(_) | ExprKind::Float(_) | ExprKind::Str(_) | ExprKind::Bool(_) | ExprKind::Undefined | ExprKind::Null | ExprKind::Ident(_) | ExprKind::This | ExprKind::Super | ExprKind::Error => false,
    }
}
