use super::*;
use crate::ast::{Arg, ArrowBody, ArrowFn, AssignOp, BinOp, ExprKind, UnOp};

pub(super) struct CallSig {
    pub tparams: Vec<u32>,
    pub params: Vec<FnParam>,
    pub names: Vec<String>,
    pub rest: Option<TyId>,
    pub ret: TyId,
}

pub(super) struct Place {
    pub ty: TyId,
    /// The place is exactly this local (`x = ...`).
    pub local: Option<LocalId>,
    /// The local the place is rooted in (`x.a[i]` → `x`).
    pub root: Option<LocalId>,
}

enum Root {
    Local(LocalId),
    ModuleConst(Sym),
    Temp,
}

impl<'a> Checker<'a> {
    pub(super) fn is_int_literal(&self, e: ExprId) -> bool {
        match &self.ast().expr(e).kind {
            ExprKind::Int(_) => true,
            ExprKind::Unary(UnOp::Neg, x) | ExprKind::Paren(x) => self.is_int_literal(*x),
            _ => false,
        }
    }

    fn numeric_hint(&mut self, exp: Option<TyId>) -> Option<TyId> {
        let exp = exp?;
        let members = self.flat_members(exp);
        if members.contains(&INT) {
            return Some(INT);
        }
        members.iter().copied().find(|&m| self.types.is_numeric(m))
    }

    pub(super) fn printable(&self, t: TyId) -> bool {
        !matches!(self.types.get(t), Ty::Func(..) | Ty::Namespace(_) | Ty::BuiltinNs(_) | Ty::Expect(_) | Ty::Void | Ty::Never)
    }

    pub(super) fn expr(&mut self, e: ExprId, exp: Option<TyId>) -> TyId {
        let t = self.expr_inner(e, exp);
        if let Some(f) = self.facts_mut() {
            f.expr_ty[e as usize] = t;
        }
        t
    }

    fn expr_inner(&mut self, e: ExprId, exp: Option<TyId>) -> TyId {
        let ast = self.ast();
        let node = ast.expr(e);
        let span = node.span;
        match &node.kind {
            ExprKind::Error => ERROR,
            ExprKind::Int(v) => {
                let t = self.numeric_hint(exp).unwrap_or(INT);
                if let Some((lo, hi)) = int_range(t)
                    && (*v as i128) > hi {
                        let msg = format!("{v} doesn't fit in `{}` (range {lo}..={hi})", self.show(t));
                        self.report(Diagnostic::new("T0020", span, msg));
                    }
                t
            }
            ExprKind::Float(_) => match self.numeric_hint(exp) {
                Some(F32) => F32,
                _ => F64,
            },
            ExprKind::Str(s) => self.types.str_lit(*s),
            ExprKind::Template(_, exprs) => {
                for &x in exprs {
                    let t = self.expr(x, None);
                    if !self.printable(t) {
                        let msg = format!("a value of type `{}` can't be interpolated into a string", self.show(t));
                        let s = self.ast().expr(x).span;
                        self.report(Diagnostic::new("T0510", s, msg));
                    }
                }
                STR
            }
            ExprKind::Bool(_) => BOOL,
            ExprKind::Undefined => UNDEFINED,
            ExprKind::Null => {
                self.report(
                    Diagnostic::new("X0002", span, "`null` is not supported; Barm has one \"absent\" value, `undefined`")
                        .fix(Applicability::Safe, "use `undefined`", span, "undefined"),
                );
                UNDEFINED
            }
            ExprKind::Ident(sym) => self.ident(e, *sym, span),
            ExprKind::Paren(x) => self.expr(*x, exp),
            ExprKind::Unary(op, x) => self.unary(*op, *x, exp, span),
            ExprKind::Binary(op, l, r) => self.binary(*op, *l, *r, exp, span),
            ExprKind::Assign(op, t, v) => self.assign(*op, *t, *v, span),
            ExprKind::Update { target, .. } => {
                let Some(place) = self.place(*target, false) else { return ERROR };
                if !self.types.is_numeric(place.ty) && place.ty != ERROR {
                    let msg = format!("`++`/`--` need a number, found `{}`", self.show(place.ty));
                    self.report(Diagnostic::new("T0501", span, msg));
                }
                place.ty
            }
            ExprKind::Call { callee, type_args, args, optional } => {
                let t = self.call(e, *callee, type_args, args, *optional, exp, span);
                if let Some(f) = self.facts_mut()
                    && let Some(c) = f.calls.get_mut(&e)
                {
                    c.ret = t;
                }
                t
            }
            ExprKind::New { callee, type_args, args } => self.new_expr(*callee, type_args, args, exp, span),
            ExprKind::Member { obj, name, name_span, optional } => self.member(e, *obj, *name, *name_span, *optional, span),
            ExprKind::Index { obj, index, optional } => self.index(*obj, *index, *optional, span),
            ExprKind::Object(fields) => self.object(fields, exp, span),
            ExprKind::Array(elems) => self.array_lit(elems, exp, span),
            ExprKind::Arrow(f) => self.arrow(f, exp, span),
            ExprKind::Cond(c, a, b) => {
                self.cond(*c);
                let pos = self.narrowings(*c, true);
                let neg = self.narrowings(*c, false);
                self.push_scope();
                self.apply(&pos);
                let ta = self.expr(*a, exp);
                self.pop_scope();
                self.push_scope();
                self.apply(&neg);
                let tb = self.expr(*b, exp.or(Some(ta)));
                self.pop_scope();
                if let Some(x) = exp
                    && self.assignable(ta, x) && self.assignable(tb, x) {
                        return self.types.union(&[ta, tb]);
                    }
                if self.types.is_int(ta) && self.types.is_float(tb) || self.types.is_float(ta) && self.types.is_int(tb) {
                    return F64;
                }
                self.types.union(&[ta, tb])
            }
            ExprKind::As(x, te) => {
                if let TypeExprKind::Named { name, ns: None, args, .. } = &self.ast().ty(*te).kind
                    && self.name(*name) == "const" && args.is_empty() {
                        return self.expr(*x, exp);
                    }
                let tscope = self.tscope();
                let target = self.resolve_type(*te, &tscope);
                // The target guides literal typing (`[] as int[]`), like an annotation would.
                let src = self.expr(*x, Some(target));
                if src == ERROR || target == ERROR || src == UNKNOWN || self.assignable(src, target) || self.assignable(target, src) {
                    return target;
                }
                let (ss, ts) = (self.show(src), self.show(target));
                let text = self.src(self.ast().expr(*x).span).to_string();
                let mut d = Diagnostic::new("T0601", span, format!("can't cast `{ss}` to `{ts}`"))
                    .note("why", "`as` only narrows (e.g. from `unknown` or a union); it never converts values");
                if self.types.is_numeric(src) && self.types.is_numeric(target) {
                    if self.types.is_int(target) && self.types.is_float(src) {
                        d = d.fix(Applicability::Maybe, format!("convert: `Math.trunc({text})`"), span, format!("Math.trunc({text})"));
                    } else {
                        d = d.fix(Applicability::Maybe, format!("convert: `{ts}({text})`"), span, format!("{ts}({text})"));
                    }
                } else if target == STR {
                    d = d.fix(Applicability::Maybe, format!("convert: `String({text})`"), span, format!("String({text})"));
                }
                self.report(d);
                target
            }
            ExprKind::NonNull(x) => {
                let hint = exp.map(|t| self.types.optional(t));
                let t = self.expr(*x, hint);
                self.types.without_undefined(t)
            }
            ExprKind::Typeof(x) => {
                self.expr(*x, None);
                STR
            }
        }
    }

    fn ident(&mut self, e: ExprId, sym: Sym, span: Span) -> TyId {
        if let Some((id, ty)) = self.lookup(sym) {
            let key = self.local(id).span.start;
            self.rec_ident(e, IdentFact::Local(key));
            self.fcx().reads.push((id, span.start));
            return ty;
        }
        match self.scopes[self.cur as usize].values.get(&sym).map(|d| d.0) {
            Some(Decl::Fn(m, i)) => self.rec_ident(e, IdentFact::Fn(m, i)),
            Some(Decl::Const(m, i)) => self.rec_ident(e, IdentFact::Const(m, i)),
            Some(Decl::Ns(m)) => self.rec_ident(e, IdentFact::Ns(m)),
            _ => self.rec_ident(e, IdentFact::Builtin),
        }
        match self.scopes[self.cur as usize].values.get(&sym).map(|d| d.0) {
            Some(Decl::Fn(m, i)) => {
                let Some(sig) = self.fn_sig(m, i) else {
                    let n = self.name(sym).to_string();
                    self.report(Diagnostic::new("T0303", span, format!("`{n}` is recursive, so it needs an explicit return type")));
                    return ERROR;
                };
                if !sig.tparams.is_empty() {
                    let n = self.name(sym).to_string();
                    self.report(
                        Diagnostic::new("T0304", span, format!("generic function `{n}` can't be used as a value"))
                            .note("instead", format!("wrap it in an arrow: `(x) => {n}(x)`")),
                    );
                    return ERROR;
                }
                return self.types.func(sig.params, sig.ret);
            }
            Some(Decl::Const(m, i)) => return self.const_type(m, i),
            Some(Decl::Ns(m)) => return self.types.intern(Ty::Namespace(m)),
            _ => {}
        }
        let text = self.name(sym).to_string();
        if builtins::is_builtin_ns(&text) {
            return self.types.intern(Ty::BuiltinNs(sym));
        }
        if let Some(msg) = builtins::removed_global(&text) {
            self.report(Diagnostic::new("X0032", span, msg));
            return ERROR;
        }
        if builtins::is_builtin_fn(&text) {
            self.report(Diagnostic::new("T0203", span, format!("built-in `{text}` must be called")));
            return ERROR;
        }
        if self.scopes[self.cur as usize].types.contains_key(&sym) {
            self.report(Diagnostic::new("N0001", span, format!("`{text}` is a type, not a value")));
            return ERROR;
        }
        let mut candidates: Vec<String> = Vec::new();
        if let Some(fcx) = self.fcx.last() {
            for scope in &fcx.scopes {
                candidates.extend(scope.iter().filter(|(s, _, _)| *s != super::PATH_SYM).map(|(s, _, _)| self.name(*s).to_string()));
            }
        }
        candidates.extend(self.scopes[self.cur as usize].values.keys().map(|&s| self.name(s).to_string()));
        candidates.extend(builtins::GLOBAL_NAMES.iter().map(|s| s.to_string()));
        let sug = similar(&text, candidates.iter().map(|s| s.as_str()));
        let mut d = Diagnostic::new("N0001", span, format!("unknown name `{text}`"));
        if let Some(first) = sug.first() {
            d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), span, first.to_string());
        }
        self.report(d);
        ERROR
    }

    fn unary(&mut self, op: UnOp, x: ExprId, exp: Option<TyId>, span: Span) -> TyId {
        match op {
            UnOp::Neg => {
                let t = self.expr(x, exp);
                if t != ERROR && !self.types.is_numeric(t) {
                    let msg = format!("unary `-` needs a number, found `{}`", self.show(t));
                    self.report(Diagnostic::new("T0501", span, msg));
                    return ERROR;
                }
                if matches!(self.types.get(t), Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64) && !self.is_int_literal(x) {
                    let msg = format!("unary `-` on unsigned `{}`", self.show(t));
                    self.report(Diagnostic::new("T0501", span, msg));
                }
                t
            }
            UnOp::Plus => {
                let t = self.expr(x, None);
                let text = self.src(self.ast().expr(x).span).to_string();
                let mut d = Diagnostic::new("X0028", span, "unary `+` is not supported");
                if self.types.is_string(t) {
                    d = d.note("instead", "parse explicitly").fix(Applicability::Maybe, format!("use `Number({text})`"), span, format!("Number({text})"));
                    self.report(d);
                    return self.types.optional(F64);
                }
                d = d.fix(Applicability::Safe, "remove the `+`", span, text);
                self.report(d);
                t
            }
            UnOp::Not => {
                self.cond(x);
                BOOL
            }
            UnOp::BitNot => {
                let t = self.expr(x, exp);
                if t != ERROR && !self.types.is_int(t) {
                    let msg = format!("`~` needs an integer, found `{}`", self.show(t));
                    self.report(Diagnostic::new("T0501", span, msg));
                    return ERROR;
                }
                t
            }
            UnOp::Void => {
                self.expr(x, None);
                self.report(Diagnostic::new("X0029", span, "the `void` operator is not supported"));
                UNDEFINED
            }
        }
    }

    /// Span of an operator between two operands, found in the source.
    fn op_span(&self, l: ExprId, r: ExprId, op: &str) -> Span {
        let (ls, rs) = (self.ast().expr(l).span, self.ast().expr(r).span);
        let text = &self.sm.get(ls.file).text[ls.end as usize..rs.start as usize];
        match text.find(op) {
            Some(i) => Span::new(ls.file, ls.end + i as u32, ls.end + (i + op.len()) as u32),
            None => ls.empty_at_end(),
        }
    }

    fn binary(&mut self, op: BinOp, l: ExprId, r: ExprId, exp: Option<TyId>, span: Span) -> TyId {
        use BinOp::*;
        match op {
            And | Or => {
                let left_ok = self.logical_operand(l, op, r);
                let n = self.narrowings(l, op == And);
                self.push_scope();
                self.apply(&n);
                if left_ok {
                    self.logical_operand(r, op, r);
                } else {
                    self.expr(r, None);
                }
                self.pop_scope();
                BOOL
            }
            Nullish => {
                let hint = exp.map(|t| self.types.optional(t));
                let tl = self.expr(l, hint);
                let inner = self.types.without_undefined(tl);
                let tr = self.expr(r, exp.or(Some(inner)));
                if self.types.is_int(inner) && self.types.is_float(tr) || self.types.is_float(inner) && self.types.is_int(tr) {
                    return F64;
                }
                let keeps_literals = exp.map(|x| self.flat_members(x).iter().any(|&m| matches!(self.types.get(m), Ty::StrLit(_)))).unwrap_or(false)
                    || matches!(self.types.get(inner), Ty::StrLit(_));
                let tr = if keeps_literals { tr } else { self.types.widen(tr) };
                self.types.union(&[inner, tr])
            }
            LooseEq | LooseNe => {
                let (bad, good) = if op == LooseEq { ("==", "===") } else { ("!=", "!==") };
                let os = self.op_span(l, r, bad);
                self.report(
                    Diagnostic::new("X0003", os, format!("`{bad}` is not supported"))
                        .note("why", "Barm has no type coercion; equality is always strict")
                        .fix(Applicability::Safe, format!("use `{good}`"), os, good),
                );
                self.equality(l, r, span)
            }
            Eq | Ne => self.equality(l, r, span),
            Lt | Gt | Le | Ge => {
                let (tl, tr) = self.numeric_pair(l, r, None);
                let ok = (self.types.is_numeric(tl) && self.types.is_numeric(tr)) || (self.types.is_string(tl) && self.types.is_string(tr));
                if !ok && tl != ERROR && tr != ERROR {
                    let msg = format!("can't compare `{}` with `{}` using `{}`", self.show(tl), self.show(tr), op.as_str());
                    let mut d = Diagnostic::new("T0501", span, msg);
                    if self.types.has_undefined(tl) || self.types.has_undefined(tr) {
                        d = d.note("why", "one side may be `undefined`; narrow it first");
                    }
                    self.report(d);
                } else if ok && self.types.is_int(tl) && self.types.is_int(tr) && tl != tr {
                    self.mixed_ints(tl, tr, span);
                }
                BOOL
            }
            Add => {
                let (tl, tr) = self.numeric_pair(l, r, exp);
                if self.types.is_string(tl) || self.types.is_string(tr) {
                    for (t, x) in [(tl, l), (tr, r)] {
                        if !self.printable(t) {
                            let msg = format!("can't concatenate `{}` to a string", self.show(t));
                            let s = self.ast().expr(x).span;
                            self.report(Diagnostic::new("T0501", s, msg));
                        }
                    }
                    return STR;
                }
                self.arith(op, tl, tr, span)
            }
            Sub | Mul | Rem | Pow => {
                let (tl, tr) = self.numeric_pair(l, r, exp);
                self.arith(op, tl, tr, span)
            }
            Div => {
                let (tl, tr) = self.numeric_pair(l, r, None);
                let t = self.arith(op, tl, tr, span);
                if t == ERROR {
                    ERROR
                } else if tl == F32 && tr == F32 {
                    F32
                } else {
                    F64
                }
            }
            BitAnd | BitOr | BitXor | Shl | Shr | UShr => {
                let (tl, tr) = self.numeric_pair(l, r, exp);
                if tl == ERROR || tr == ERROR {
                    return ERROR;
                }
                if !self.types.is_int(tl) || !self.types.is_int(tr) {
                    let msg = format!("`{}` needs integers, found `{}` and `{}`", op.as_str(), self.show(tl), self.show(tr));
                    self.report(Diagnostic::new("T0501", span, msg).note("instead", "convert floats first with `Math.trunc(x)`"));
                    return ERROR;
                }
                if tl != tr && !matches!(op, Shl | Shr | UShr) {
                    self.mixed_ints(tl, tr, span);
                }
                tl
            }
            In => {
                self.expr(l, None);
                self.expr(r, None);
                self.report(
                    Diagnostic::new("X0025", span, "the `in` operator is not supported")
                        .note("instead", "records have fixed fields; for dynamic keys use `map.has(key)`"),
                );
                BOOL
            }
            Instanceof => {
                self.expr(l, None);
                self.report(Diagnostic::new("U0001", span, "`instanceof` needs classes, which are not supported yet (planned for M2)"));
                BOOL
            }
        }
    }

    /// Returns false if an error was reported.
    fn logical_operand(&mut self, e: ExprId, op: BinOp, rhs: ExprId) -> bool {
        let t = self.expr(e, Some(BOOL));
        if t == BOOL || t == ERROR || self.condition_ok(t) {
            return true;
        }
        let span = self.ast().expr(e).span;
        let shown = self.show(t);
        let mut d = Diagnostic::new("T0401", span, format!("`{}` needs `bool` operands, found `{shown}`", op.as_str()))
            .note("why", "no implicit truthiness: `0`, `\"\"` and `undefined` are not `false`");
        if op == BinOp::Or && self.types.has_undefined(t) && rhs != e {
            let os = self.op_span(e, rhs, "||");
            d = d.fix(Applicability::Maybe, "for a default value, use `??`", os, "??");
        }
        self.report(d);
        false
    }

    /// Checks two operands so an integer literal on either side adopts the other side's numeric type.
    fn numeric_pair(&mut self, l: ExprId, r: ExprId, exp: Option<TyId>) -> (TyId, TyId) {
        if self.is_int_literal(l) && !self.is_int_literal(r) {
            let tr = self.expr(r, exp);
            let hint = if self.types.is_numeric(tr) { Some(tr) } else { exp };
            let tl = self.expr(l, hint);
            (tl, tr)
        } else {
            let tl = self.expr(l, exp);
            let hint = if self.types.is_numeric(tl) { Some(tl) } else { None };
            let tr = self.expr(r, hint);
            (tl, tr)
        }
    }

    fn mixed_ints(&mut self, a: TyId, b: TyId, span: Span) {
        let msg = format!("mixed integer types `{}` and `{}`", self.show(a), self.show(b));
        self.report(Diagnostic::new("T0504", span, msg).note("instead", "convert one side explicitly, e.g. `int(x)`"));
    }

    fn arith(&mut self, op: BinOp, tl: TyId, tr: TyId, span: Span) -> TyId {
        if tl == ERROR || tr == ERROR {
            return ERROR;
        }
        if !self.types.is_numeric(tl) || !self.types.is_numeric(tr) {
            let msg = format!("can't apply `{}` to `{}` and `{}`", op.as_str(), self.show(tl), self.show(tr));
            let mut d = Diagnostic::new("T0501", span, msg);
            if self.types.has_undefined(tl) || self.types.has_undefined(tr) {
                d = d.note("why", "one side may be `undefined`; narrow it or use `?? 0`");
            }
            self.report(d);
            return ERROR;
        }
        if tl == tr {
            return tl;
        }
        let (fl, fr) = (self.types.is_float(tl), self.types.is_float(tr));
        match (fl, fr) {
            (true, true) => F64,
            (true, false) if tr == INT => tl,
            (false, true) if tl == INT => tr,
            (false, false) => {
                self.mixed_ints(tl, tr, span);
                ERROR
            }
            _ => {
                let msg = format!("can't mix `{}` and `{}` implicitly", self.show(tl), self.show(tr));
                self.report(Diagnostic::new("T0504", span, msg).note("instead", "convert the integer with `f64(x)`"));
                ERROR
            }
        }
    }

    fn equality(&mut self, l: ExprId, r: ExprId, span: Span) -> TyId {
        let (tl, tr) = if matches!(self.ast().expr(l).kind, ExprKind::Str(_) | ExprKind::Undefined) {
            let tr = self.expr(r, None);
            let tl = self.expr(l, Some(tr));
            (tl, tr)
        } else {
            let tl = self.expr(l, None);
            let tr = self.expr(r, if self.types.is_numeric(tl) { Some(tl) } else { None });
            (tl, tr)
        };
        let undefined_literal = matches!(self.ast().expr(l).kind, ExprKind::Undefined) || matches!(self.ast().expr(r).kind, ExprKind::Undefined);
        if !undefined_literal && !self.comparable(tl, tr) {
            let (ls, rs) = (self.show(tl), self.show(tr));
            let mut d = Diagnostic::new("T0502", span, format!("this comparison is always false: `{ls}` and `{rs}` have no values in common"));
            // `s.kind === "circel"` → suggest the literal that exists.
            let (lit, other, lit_span) = match (*self.types.get(tl), *self.types.get(tr)) {
                (_, Ty::StrLit(s)) => (Some(s), tl, self.ast().expr(r).span),
                (Ty::StrLit(s), _) => (Some(s), tr, self.ast().expr(l).span),
                _ => (None, tl, span),
            };
            if let Some(lit) = lit {
                let options: Vec<String> = self.flat_members(other).into_iter().filter_map(|m| match self.types.get(m) {
                    Ty::StrLit(s) => Some(self.name(*s).to_string()),
                    _ => None,
                }).collect();
                if !options.is_empty() {
                    let lit_text = self.name(lit).to_string();
                    let quoted: Vec<String> = options.iter().map(|o| format!("\"{o}\"")).collect();
                    d = d.note("valid values", quoted.join(", "));
                    let sug = similar(&lit_text, options.iter().map(|s| s.as_str()));
                    if let Some(first) = sug.first() {
                        d = d.fix(Applicability::Maybe, format!("use \"{first}\""), lit_span, format!("\"{first}\""));
                    }
                }
            }
            self.report(d);
        }
        BOOL
    }

    fn assign(&mut self, op: AssignOp, target: ExprId, value: ExprId, span: Span) -> TyId {
        let t = self.assign_inner(op, target, value, span);
        // After the value: `cur = cur.left` reads the narrowed `cur.left` first.
        self.invalidate_place(target);
        t
    }

    fn assign_inner(&mut self, op: AssignOp, target: ExprId, value: ExprId, span: Span) -> TyId {
        let Some(place) = self.place(target, false) else {
            self.expr(value, None);
            return ERROR;
        };
        let vspan = self.ast().expr(value).span;
        match op {
            AssignOp::Assign => {
                let tv = self.expr(value, Some(place.ty));
                if !self.try_promote(&place, tv) {
                    self.expect_assignable(tv, place.ty, vspan, None);
                }
                if let Some(local) = place.local
                    && self.assignable(tv, place.ty) && matches!(self.types.get(self.local(local).ty), Ty::Union(_)) {
                        self.narrow(local, tv);
                    }
            }
            AssignOp::Op(BinOp::Nullish) => {
                if !self.types.has_undefined(place.ty) && place.ty != ERROR {
                    let msg = format!("`??=` needs an optional target, found `{}`", self.show(place.ty));
                    self.report(Diagnostic::new("T0503", span, msg));
                }
                let inner = self.types.without_undefined(place.ty);
                let tv = self.expr(value, Some(inner));
                self.expect_assignable(tv, inner, vspan, None);
                if let Some(local) = place.local {
                    self.narrow(local, inner);
                }
            }
            AssignOp::Op(BinOp::And | BinOp::Or) => {
                self.report(Diagnostic::new("X0026", span, "`&&=` and `||=` are not supported; write the `if` explicitly"));
                self.expr(value, None);
            }
            AssignOp::Op(bop) => {
                let hint = if self.types.is_numeric(place.ty) { Some(place.ty) } else { None };
                let tv = self.expr(value, hint);
                let result = if bop == BinOp::Add && self.types.is_string(place.ty) {
                    if !self.printable(tv) {
                        let msg = format!("can't concatenate `{}` to a string", self.show(tv));
                        self.report(Diagnostic::new("T0501", vspan, msg));
                    }
                    STR
                } else if matches!(bop, BinOp::BitAnd | BinOp::BitOr | BinOp::BitXor | BinOp::Shl | BinOp::Shr | BinOp::UShr) {
                    if place.ty != ERROR && tv != ERROR && (!self.types.is_int(place.ty) || !self.types.is_int(tv)) {
                        let msg = format!("`{}=` needs integers, found `{}` and `{}`", bop.as_str(), self.show(place.ty), self.show(tv));
                        self.report(Diagnostic::new("T0501", span, msg));
                    }
                    place.ty
                } else {
                    let r = self.arith(bop, place.ty, tv, span);
                    if bop == BinOp::Div && r != ERROR { if r == F32 { F32 } else { F64 } } else { r }
                };
                if !self.try_promote(&place, result)
                    && result != ERROR && !self.assignable(result, place.ty) {
                        let msg = format!("`{}=` produces `{}`, which can't be stored in `{}`", bop.as_str(), self.show(result), self.show(place.ty));
                        let mut d = Diagnostic::new("T0001", span, msg);
                        if self.types.is_int(place.ty) && self.types.is_float(result) {
                            d = d.note("why", "the variable is an `int`; declare it as `f64` (e.g. `let x = 0.0`) or round the result");
                        }
                        self.report(d);
                    }
            }
        }
        place.ty
    }

    /// `let x = 0; x += 0.5` — records that `x` should be an `f64` and re-checks the function.
    fn try_promote(&mut self, place: &Place, value: TyId) -> bool {
        let Some(local) = place.local else { return false };
        let l = self.local(local);
        if l.promotable && l.ty == INT && self.types.is_float(value) {
            let key = l.span.start;
            self.fcx().pending.insert(key);
            return true;
        }
        false
    }

    fn root_of(&self, e: ExprId) -> Root {
        match &self.ast().expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) => self.root_of(*x),
            ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } => self.root_of(*obj),
            ExprKind::Ident(s) => match self.lookup(*s) {
                Some((id, _)) => Root::Local(id),
                None => match self.scopes[self.cur as usize].values.get(s).map(|d| d.0) {
                    Some(Decl::Const(..)) => Root::ModuleConst(*s),
                    _ => Root::Temp,
                },
            },
            _ => Root::Temp,
        }
    }

    /// Reports an error if the value rooted at `e` can't be modified in place.
    pub(super) fn check_mutable_root(&mut self, e: ExprId, span: Span) -> Option<LocalId> {
        self.check_mutable_root_ext(e, span, false)
    }

    /// `allow_temp`: mutating a temporary (`xs.slice().sort()`) is fine for method calls.
    pub(super) fn check_mutable_root_ext(&mut self, e: ExprId, span: Span, allow_temp: bool) -> Option<LocalId> {
        match self.root_of(e) {
            Root::Temp if allow_temp => None,
            Root::Local(id) => {
                let (kind, name, lspan) = {
                    let l = self.local(id);
                    (l.kind, l.name, l.span)
                };
                let n = self.name(name).to_string();
                match kind {
                    LocalKind::Param => {
                        self.report(
                            Diagnostic::new("V0101", span, format!("can't modify parameter `{n}`: parameters are read-only"))
                                .note("to change the caller's value", format!("declare `inout {n}` and call with `&arg`"))
                                .note("to change a local copy", format!("`let copy = {n}` and modify `copy`"))
                                .fix(Applicability::Maybe, format!("make `{n}` an `inout` parameter"), lspan.empty_at_start(), "inout "),
                        );
                        None
                    }
                    LocalKind::LoopVar => {
                        self.report(
                            Diagnostic::new("V0104", span, format!("can't modify loop variable `{n}`: it is a copy of the element"))
                                .note("why", "changes to the copy would not reach the collection (Barm collections hold values)")
                                .note("instead", "loop over indices and assign `xs[i] = ...`, or build a new array with `map`"),
                        );
                        None
                    }
                    _ => {
                        self.fcx().mutations.push((id, span));
                        Some(id)
                    }
                }
            }
            Root::ModuleConst(s) => {
                let n = self.name(s).to_string();
                self.report(Diagnostic::new("V0102", span, format!("can't modify module constant `{n}`")));
                None
            }
            Root::Temp => {
                self.report(Diagnostic::new("V0103", span, "can't modify a temporary value").note("instead", "assign it to a variable first"));
                None
            }
        }
    }

    pub(super) fn place(&mut self, e: ExprId, _for_ref: bool) -> Option<Place> {
        let node = self.ast().expr(e);
        let span = node.span;
        match &node.kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) => self.place(*x, _for_ref),
            ExprKind::Ident(sym) => {
                if let Some((id, ty)) = self.lookup(*sym) {
                    let key = self.local(id).span.start;
                    self.rec_ident(e, IdentFact::Local(key));
                    let declared = self.fcx.last().unwrap().locals[id].ty;
                    if let Some(f) = self.facts_mut() {
                        f.expr_ty[e as usize] = declared;
                    }
                    let (kind, kw_span) = (self.local(id).kind, self.local(id).kw_span);
                    let declared = self.local(id).ty;
                    let n = self.name(*sym).to_string();
                    if kind == LocalKind::Const {
                        let mut d = if _for_ref {
                            Diagnostic::new("V0001", span, format!("`{n}` is a `const`, so it can't be passed as `inout`"))
                                .note("why", "the callee may replace the whole value, which would change a constant")
                        } else {
                            Diagnostic::new("V0001", span, format!("can't reassign constant `{n}`"))
                        };
                        if let Some(kw) = kw_span {
                            d = d.fix(Applicability::Safe, format!("declare `{n}` with `let`"), kw, "let");
                        }
                        self.report(d);
                        return None;
                    }
                    if kind == LocalKind::LoopVar {
                        self.report(Diagnostic::new("V0001", span, format!("can't reassign loop variable `{n}`")));
                        return None;
                    }
                    let _ = ty;
                    return Some(Place { ty: declared, local: Some(id), root: Some(id) });
                }
                let n = self.name(*sym).to_string();
                match self.scopes[self.cur as usize].values.get(sym).map(|d| d.0) {
                    Some(Decl::Const(..)) => {
                        self.report(Diagnostic::new("V0102", span, format!("can't reassign module constant `{n}`")));
                        None
                    }
                    Some(Decl::Fn(..)) => {
                        self.report(Diagnostic::new("V0105", span, format!("can't assign to function `{n}`")));
                        None
                    }
                    _ => {
                        self.ident(e, *sym, span);
                        None
                    }
                }
            }
            ExprKind::Member { obj, name, name_span, optional } => {
                if *optional {
                    self.report(Diagnostic::new("V0103", span, "can't assign through `?.`").note("instead", "narrow the value first: `if (x !== undefined) x.field = v`"));
                    return None;
                }
                let n = self.name(*name).to_string();
                let t = self.expr(*obj, None);
                if n == "length" && matches!(self.types.get(t), Ty::Array(_)) {
                    self.report(Diagnostic::new("X0034", span, "assigning an array's `length` is not supported").note("instead", "reassign: `xs = []`, or `xs = xs.slice(0, n)`"));
                    return None;
                }
                let ft = self.member_of(t, *name, *name_span, span, *obj)?;
                let root = self.check_mutable_root(*obj, span)?;
                Some(Place { ty: ft, local: None, root: Some(root) })
            }
            ExprKind::Index { obj, index, optional } => {
                if *optional {
                    self.report(Diagnostic::new("V0103", span, "can't assign through `?.[]`"));
                    return None;
                }
                let t = self.expr(*obj, None);
                let elem = match *self.types.get(t) {
                    Ty::Array(e) => {
                        self.index_type(*index);
                        e
                    }
                    Ty::Error => return None,
                    Ty::Map(..) => {
                        let (o, i) = (self.src(self.ast().expr(*obj).span).to_string(), self.src(self.ast().expr(*index).span).to_string());
                        self.report(
                            Diagnostic::new("X0017", span, "maps are not indexed with `[]`")
                                .note("instead", format!("`{o}.set({i}, value)`")),
                        );
                        return None;
                    }
                    _ => {
                        let msg = format!("can't index into `{}`", self.show(t));
                        self.report(Diagnostic::new("T0110", span, msg));
                        return None;
                    }
                };
                let root = self.check_mutable_root(*obj, span)?;
                Some(Place { ty: elem, local: None, root: Some(root) })
            }
            _ => {
                self.expr(e, None);
                self.report(Diagnostic::new("V0103", span, "this expression can't be assigned to"));
                None
            }
        }
    }

    fn index_type(&mut self, index: ExprId) {
        let ti = self.expr(index, Some(INT));
        if ti == ERROR || self.types.is_int(ti) {
            return;
        }
        let span = self.ast().expr(index).span;
        let text = self.src(span).to_string();
        let mut d = Diagnostic::new("T0111", span, format!("array index must be an integer, found `{}`", self.show(ti)));
        if self.types.is_float(ti) {
            d = d.fix(Applicability::Maybe, format!("use `Math.floor({text})`"), span, format!("Math.floor({text})"));
        }
        self.report(d);
    }

    fn index(&mut self, obj: ExprId, index: ExprId, optional: bool, span: Span) -> TyId {
        let t = self.expr(obj, None);
        let base = if optional {
            self.types.without_undefined(t)
        } else {
            if self.types.has_undefined(t) {
                let d = self.possibly_undefined(obj, span);
                self.report(d);
            }
            self.types.without_undefined(t)
        };
        let result = match *self.types.get(base) {
            Ty::Error => return ERROR,
            Ty::Array(e) => {
                self.index_type(index);
                self.types.optional(e)
            }
            Ty::Map(..) => {
                self.expr(index, None);
                let (o, i) = (self.src(self.ast().expr(obj).span).to_string(), self.src(self.ast().expr(index).span).to_string());
                self.report(
                    Diagnostic::new("X0017", span, "maps are not indexed with `[]`")
                        .fix(Applicability::Safe, format!("use `{o}.get({i})`"), span, format!("{o}.get({i})")),
                );
                return ERROR;
            }
            Ty::Str | Ty::StrLit(_) => {
                self.expr(index, None);
                self.report(
                    Diagnostic::new("X0035", span, "strings can't be indexed by position (they are UTF-8)")
                        .note("instead", "`s.chars()[i]` for the i-th character, or `s.slice(start, end)`"),
                );
                return ERROR;
            }
            Ty::Record(fields) => {
                if let ExprKind::Str(key) = self.ast().expr(index).kind {
                    let key = &self.name(key).to_string();
                    let o = self.src(self.ast().expr(obj).span).to_string();
                    if self.types.fields(fields).iter().any(|f| self.name(f.name) == key) {
                        let key = key.clone();
                        self.report(
                            Diagnostic::new("X0017", span, "records are accessed with `.field`, not `[\"field\"]`")
                                .fix(Applicability::Safe, format!("use `{o}.{key}`"), span, format!("{o}.{key}")),
                        );
                        return ERROR;
                    }
                }
                self.expr(index, None);
                self.report(Diagnostic::new("X0017", span, "records can't be indexed").note("instead", "use `.field`, or a `Map<K, V>` for dynamic keys"));
                return ERROR;
            }
            _ => {
                self.expr(index, None);
                let msg = format!("can't index into `{}`", self.show(base));
                self.report(Diagnostic::new("T0110", span, msg));
                return ERROR;
            }
        };
        if optional { self.types.optional(result) } else { result }
    }

    pub(super) fn possibly_undefined(&mut self, obj: ExprId, span: Span) -> Diagnostic {
        let ospan = self.ast().expr(obj).span;
        let text = self.src(ospan).to_string();
        let rest = self.sm.get(span.file).text[ospan.end as usize..span.end as usize].to_string();
        let (chain, insert) = if rest.starts_with('.') { (format!("{text}?{rest}"), "?") } else { (format!("{text}?.{rest}"), "?.") };
        Diagnostic::new("T0120", ospan, format!("`{text}` may be `undefined`"))
            .note("narrow first", format!("`if ({text} !== undefined) {{ ... }}`"))
            .fix(Applicability::Maybe, format!("optional chaining: `{chain}`"), ospan.empty_at_end(), insert)
            .fix(Applicability::Maybe, format!("assert defined (traps if not): `{text}!{rest}`"), ospan.empty_at_end(), "!")
    }

    fn member(&mut self, e: ExprId, obj: ExprId, name: Sym, name_span: Span, optional: bool, span: Span) -> TyId {
        // Namespaces: `ns.value`, `Math.PI`.
        if let ExprKind::Ident(s) = self.ast().expr(obj).kind
            && self.lookup(s).is_none() {
                match self.scopes[self.cur as usize].values.get(&s).map(|d| d.0) {
                    Some(Decl::Ns(m)) => {
                        let scope = &self.scopes[m as usize];
                        match scope.values.get(&name).map(|d| d.0) {
                            Some(Decl::Fn(fm, fi)) => {
                                if let Some(f) = self.facts_mut() {
                                    f.members.insert(e, MemberFact::NsFn(fm, fi));
                                }
                            }
                            Some(Decl::Const(cm, ci)) => {
                                if let Some(f) = self.facts_mut() {
                                    f.members.insert(e, MemberFact::NsConst(cm, ci));
                                }
                            }
                            _ => {}
                        }
                        return self.ns_value(m, name, name_span);
                    }
                    None => {
                        let text = self.name(s).to_string();
                        if builtins::is_builtin_ns(&text) {
                            if let Some(f) = self.facts_mut() {
                                f.members.insert(e, MemberFact::MathConst);
                            }
                            return self.builtin_ns_member(&text, name, name_span);
                        }
                    }
                    _ => {}
                }
            }
        let t = self.expr(obj, None);
        if t == ERROR {
            return ERROR;
        }
        let base = if optional {
            self.types.without_undefined(t)
        } else {
            if self.types.has_undefined(t) {
                let d = self.possibly_undefined(obj, span);
                self.report(d);
            }
            self.types.without_undefined(t)
        };
        let Some(ft) = self.member_of(base, name, name_span, span, obj) else { return ERROR };
        if !optional
            && let Some((target, _)) = self.target_of(obj)
        {
            let mut path = target.path.clone();
            path.push(name);
            if let Some(t) = self.lookup_path(target.local, &path) {
                return t;
            }
        }
        if optional { self.types.optional(ft) } else { ft }
    }

    fn ns_value(&mut self, m: u32, name: Sym, name_span: Span) -> TyId {
        let scope = &self.scopes[m as usize];
        let decl = scope.values.get(&name).filter(|_| scope.exported_values.contains(&name)).map(|d| d.0);
        match decl {
            Some(Decl::Fn(fm, fi)) => match self.fn_sig(fm, fi) {
                Some(sig) if sig.tparams.is_empty() => self.types.func(sig.params, sig.ret),
                Some(_) => {
                    let n = self.name(name).to_string();
                    self.report(Diagnostic::new("T0304", name_span, format!("generic function `{n}` must be called")));
                    ERROR
                }
                None => ERROR,
            },
            Some(Decl::Const(cm, ci)) => self.const_type(cm, ci),
            _ => {
                let n = self.name(name).to_string();
                let mname = self.modules[m as usize].name.clone();
                let exports: Vec<String> = self.scopes[m as usize].exported_values.iter().map(|&s| self.name(s).to_string()).collect();
                let mut d = Diagnostic::new("N0102", name_span, format!("`{mname}` has no exported value `{n}`"));
                let sug = similar(&n, exports.iter().map(|s| s.as_str()));
                if let Some(first) = sug.first() {
                    d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), name_span, first.to_string());
                }
                self.report(d);
                ERROR
            }
        }
    }

    /// Type of `base.name` for a non-optional base.
    pub(super) fn member_of(&mut self, base: TyId, name: Sym, name_span: Span, span: Span, obj: ExprId) -> Option<TyId> {
        let n = self.name(name).to_string();
        let members = self.flat_members(base);
        let mut found = Vec::new();
        let mut lacking = Vec::new();
        for &m in &members {
            match self.field_of(m, name) {
                Some(t) => found.push(t),
                None => lacking.push(m),
            }
        }
        if lacking.is_empty() && !found.is_empty() {
            return Some(self.types.union(&found));
        }
        if let Some(msg) = builtins::member_hint(&self.types, base, &n) {
            self.report(msg.into_diag(name_span));
            return None;
        }
        if builtins::is_method(self, base, &n) {
            self.report(Diagnostic::new("T0203", name_span, format!("method `{n}` must be called: `{n}(...)`")));
            return None;
        }
        let shown = self.show(base);
        if !found.is_empty() {
            // Some union members have the field: suggest narrowing.
            let obj_text = self.src(self.ast().expr(obj).span).to_string();
            let mut d = Diagnostic::new("T0108", name_span, format!("not every variant of `{shown}` has field `{n}`"));
            let disc = self.discriminant_values(&lacking);
            if !disc.is_empty() {
                d = d.note("missing on", disc.join(", "));
            }
            d = d.note("narrow first", format!("`switch ({obj_text}.kind)` or `if ({obj_text}.kind === ...)`"));
            self.report(d);
            let _ = span;
            return None;
        }
        let names = self.field_names(base);
        let is_record = self.flat_members(base).iter().all(|&m| matches!(self.types.get(m), Ty::Record(_) | Ty::Interface(..)));
        let what = if is_record { "field" } else { "member" };
        let mut d = Diagnostic::new("T0107", name_span, format!("`{shown}` has no {what} `{n}`"));
        let sug = similar(&n, names.iter().map(|s| s.as_str()));
        if let Some(first) = sug.first() {
            d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), name_span, first.to_string());
        }
        if !names.is_empty() {
            d = d.note("available", names.join(", "));
        }
        self.report(d);
        None
    }

    fn discriminant_values(&mut self, members: &[TyId]) -> Vec<String> {
        let kind = self.syms.kind;
        let mut out = Vec::new();
        for &m in members {
            if let Ty::Record(fs) = *self.types.get(m)
                && let Some(f) = self.types.fields(fs).iter().find(|f| f.name == kind)
                    && let Ty::StrLit(s) = self.types.get(f.ty) {
                        out.push(format!("\"{}\"", self.name(*s)));
                    }
        }
        out
    }

    fn field_of(&mut self, ty: TyId, name: Sym) -> Option<TyId> {
        let n = self.name(name).to_string();
        match *self.types.get(ty) {
            Ty::Error => Some(ERROR),
            Ty::Record(fs) => self.types.fields(fs).iter().find(|f| f.name == name).map(|f| f.ty),
            Ty::Interface(i, args) => {
                let args = self.types.tys(args).to_vec();
                self.iface_fields_inst(i, &args).into_iter().find(|f| f.name == name).map(|f| f.ty)
            }
            Ty::Param(p) => {
                let b = self.gparam(p).bound?;
                self.field_of(b, name)
            }
            _ => builtins::property(&self.types, ty, &n),
        }
    }

    fn field_names(&mut self, ty: TyId) -> Vec<String> {
        let mut out = Vec::new();
        for m in self.flat_members(ty) {
            match *self.types.get(m) {
                Ty::Record(fs) => out.extend(self.types.fields(fs).iter().map(|f| self.name(f.name).to_string())),
                Ty::Interface(i, args) => {
                    let args = self.types.tys(args).to_vec();
                    let fs = self.iface_fields_inst(i, &args);
                    out.extend(fs.iter().map(|f| self.name(f.name).to_string()));
                }
                _ => out.extend(builtins::property_names(self, m).into_iter().map(|s| s.to_string())),
            }
        }
        out.sort();
        out.dedup();
        out
    }

    fn object(&mut self, fields: &[crate::ast::ObjField], exp: Option<TyId>, span: Span) -> TyId {
        let mut seen = HashSet::default();
        for f in fields {
            if !seen.insert(f.name) {
                let n = self.name(f.name).to_string();
                self.report(Diagnostic::new("N0006", f.name_span, format!("duplicate field `{n}`")));
            }
        }
        let target = exp.and_then(|e| self.pick_record(fields, e, span));
        if let Some((ERROR, _)) = target {
            return ERROR;
        }
        let Some((target_ty, tfields)) = target else {
            if exp.map(|e| self.types.get(e) == &Ty::Error).unwrap_or(false) {
                for f in fields {
                    self.expr(f.value, None);
                }
                return ERROR;
            }
            // No usable context: infer a record type, widening string literals as TS does.
            let mut out = Vec::new();
            for f in fields {
                let t = self.expr(f.value, None);
                let t = self.types.widen(t);
                out.push(Field { name: f.name, ty: t, optional: false });
            }
            out.dedup_by_key(|f| f.name);
            let order: Vec<Sym> = out.iter().map(|f| f.name).collect();
            let t = self.types.record(out);
            self.types.note_field_order(t, order);
            return t;
        };
        let mut provided = HashSet::default();
        for f in fields {
            match tfields.iter().find(|tf| tf.name == f.name) {
                Some(tf) => {
                    provided.insert(f.name);
                    let t = self.expr(f.value, Some(tf.ty));
                    let fname = self.name(f.name).to_string();
                    let vspan = self.ast().expr(f.value).span;
                    self.expect_assignable(t, tf.ty, vspan, Some(format!("field `{fname}`")));
                }
                None => {
                    self.expr(f.value, None);
                    let n = self.name(f.name).to_string();
                    let missing: Vec<String> = tfields.iter().filter(|tf| !fields.iter().any(|f| f.name == tf.name)).map(|tf| self.name(tf.name).to_string()).collect();
                    let shown = self.show(target_ty);
                    let mut d = Diagnostic::new("T0102", f.name_span, format!("`{shown}` has no field `{n}`"));
                    let sug = similar(&n, missing.iter().map(|s| s.as_str()));
                    if let Some(first) = sug.first() {
                        d = d.fix(Applicability::Maybe, format!("rename to `{first}`"), f.name_span, first.to_string());
                    }
                    let all: Vec<String> = tfields.iter().map(|tf| self.name(tf.name).to_string()).collect();
                    d = d.note("fields", all.join(", "));
                    self.report(d);
                }
            }
        }
        let missing: Vec<&Field> = tfields.iter().filter(|tf| !tf.optional && !provided.contains(&tf.name)).collect();
        // Don't report missing fields when a misspelled field already explains it.
        let misspelled = fields.iter().any(|f| !tfields.iter().any(|tf| tf.name == f.name));
        if !missing.is_empty() && !misspelled {
            let names: Vec<String> = missing.iter().map(|f| format!("`{}`", self.name(f.name))).collect();
            let insert: Vec<String> = missing.iter().map(|f| format!("{}: <{}>", self.name(f.name), self.show(f.ty))).collect();
            let shown = self.show(target_ty);
            let close = Span::new(span.file, span.end.saturating_sub(1), span.end.saturating_sub(1));
            let sep = if fields.is_empty() { " " } else { ", " };
            let d = Diagnostic::new("T0101", span, format!("missing field{} {} for `{shown}`", if missing.len() == 1 { "" } else { "s" }, names.join(", ")))
                .fix(Applicability::Placeholder, format!("add {}", insert.join(", ")), close, format!("{sep}{} ", insert.join(", ")));
            self.report(d);
        }
        target_ty
    }

    /// Chooses the record type an object literal should be checked against.
    fn pick_record(&mut self, fields: &[crate::ast::ObjField], exp: TyId, span: Span) -> Option<(TyId, Vec<Field>)> {
        let members = self.flat_members(exp);
        let records: Vec<(TyId, Vec<Field>)> = members
            .iter()
            .filter_map(|&m| match *self.types.get(m) {
                Ty::Record(fs) => Some((m, self.types.fields(fs).to_vec())),
                _ => None,
            })
            .collect();
        if records.is_empty() {
            if let Some(&m) = members.iter().find(|&&m| matches!(self.types.get(m), Ty::Interface(..)))
                && let Ty::Interface(i, args) = *self.types.get(m) {
                    let args = self.types.tys(args).to_vec();
                    let fs = self.iface_fields_inst(i, &args);
                    return Some((m, fs));
                }
            return None;
        }
        if records.len() == 1 {
            let (m, fs) = records.into_iter().next().unwrap();
            let named = if members.len() == 1 { exp } else { m };
            return Some((named, fs));
        }
        // Discriminated union: find a literal-valued field in the object that selects a variant.
        for f in fields {
            let ExprKind::Str(val_sym) = self.ast().expr(f.value).kind else { continue };
            let val = &self.name(val_sym).to_string();
            let is_disc = records.iter().all(|(_, fs)| fs.iter().any(|x| x.name == f.name && matches!(self.types.get(x.ty), Ty::StrLit(_))));
            if !is_disc {
                continue;
            }
            let lit = self.types.str_lit(val_sym);
            if let Some((m, fs)) = records.iter().find(|(_, fs)| fs.iter().any(|x| x.name == f.name && x.ty == lit)) {
                return Some((*m, fs.clone()));
            }
            let options: Vec<String> = records
                .iter()
                .filter_map(|(_, fs)| fs.iter().find(|x| x.name == f.name))
                .filter_map(|x| match self.types.get(x.ty) {
                    Ty::StrLit(s) => Some(self.name(*s).to_string()),
                    _ => None,
                })
                .collect();
            let fname = self.name(f.name).to_string();
            let shown = self.show(exp);
            let vspan = self.ast().expr(f.value).span;
            let quoted: Vec<String> = options.iter().map(|o| format!("\"{o}\"")).collect();
            let mut d = Diagnostic::new("T0103", vspan, format!("`{fname}: \"{val}\"` is not a variant of `{shown}`")).note("valid values", quoted.join(", "));
            let sug = similar(val, options.iter().map(|s| s.as_str()));
            if let Some(first) = sug.first() {
                d = d.fix(Applicability::Maybe, format!("use \"{first}\""), vspan, format!("\"{first}\""));
            }
            self.report(d);
            for f in fields {
                self.expr(f.value, None);
            }
            let _ = span;
            return Some((ERROR, Vec::new()));
        }
        None
    }

    fn array_lit(&mut self, elems: &[ExprId], exp: Option<TyId>, span: Span) -> TyId {
        let elem_exp = exp.and_then(|e| {
            self.flat_members(e).into_iter().find_map(|m| match self.types.get(m) {
                Ty::Array(t) => Some(*t),
                _ => None,
            })
        });
        if let Some(et) = elem_exp {
            for &x in elems {
                let t = self.expr(x, Some(et));
                let s = self.ast().expr(x).span;
                self.expect_assignable(t, et, s, None);
            }
            return self.types.array(et);
        }
        if elems.is_empty() {
            if exp == Some(ERROR) {
                return ERROR;
            }
            self.report(
                Diagnostic::new("T0301", span, "can't infer the element type of an empty array")
                    .note("instead", "annotate the binding: `const xs: T[] = []`"),
            );
            return ERROR;
        }
        let mut tys = Vec::new();
        for &x in elems {
            let t = self.expr(x, None);
            tys.push(self.types.widen(t));
        }
        if tys.iter().all(|&t| self.types.is_numeric(t)) && tys.iter().any(|&t| self.types.is_float(t)) && tys.iter().all(|&t| t == INT || t == F64) {
            return self.types.array(F64);
        }
        let u = self.types.union(&tys);
        self.types.array(u)
    }

    pub(super) fn arrow(&mut self, f: &ArrowFn, exp: Option<TyId>, span: Span) -> TyId {
        let exp_fn = exp.and_then(|e| {
            self.flat_members(e).into_iter().find_map(|m| match *self.types.get(m) {
                Ty::Func(ps, r) => Some((self.types.params(ps).to_vec(), r)),
                _ => None,
            })
        });
        let tscope = self.tscope();
        let mut params = Vec::new();
        for (i, p) in f.params.iter().enumerate() {
            let ty = match p.ty {
                Some(t) => self.resolve_type(t, &tscope),
                None => match exp_fn.as_ref().and_then(|(ps, _)| ps.get(i)) {
                    Some(fp) if !self.types.mentions(fp.ty, &self.infer_free) => fp.ty,
                    _ => {
                        if exp != Some(ERROR) {
                            let n = self.name(p.name).to_string();
                            self.report(
                                Diagnostic::new("T0301", p.span, format!("can't infer the type of parameter `{n}`"))
                                    .fix(Applicability::Placeholder, format!("annotate `{n}: T`"), p.span.empty_at_end(), ": T"),
                            );
                        }
                        ERROR
                    }
                },
            };
            let ty = if p.optional { self.types.optional(ty) } else { ty };
            params.push(FnParam { ty, inout: p.inout, optional: p.optional });
        }
        let declared_ret = f.ret.map(|t| self.resolve_type(t, &tscope));
        let exp_ret = exp_fn.as_ref().map(|(_, r)| *r).filter(|&r| r != VOID && !self.types.mentions(r, &self.infer_free));
        let ret = declared_ret.or(exp_ret);
        let fcx = self.fcx.last_mut().unwrap();
        fcx.frames.push(Frame { ret: declared_ret, returns: Vec::new(), loops: 0, switches: 0, name: None });
        fcx.scopes.push(Vec::new());
        for (p, fp) in f.params.iter().zip(&params) {
            let kind = if p.inout { LocalKind::Inout } else { LocalKind::Param };
            self.declare(p.name, fp.ty, kind, p.span, None, false);
        }
        let body_ty = match &f.body {
            ArrowBody::Expr(x) => {
                let t = self.expr(*x, ret);
                if let Some(r) = declared_ret {
                    let s = self.ast().expr(*x).span;
                    self.expect_assignable(t, r, s, None);
                    r
                } else if let Some(r) = exp_ret {
                    if self.assignable(t, r) { r } else { t }
                } else {
                    t
                }
            }
            ArrowBody::Block(b) => {
                if declared_ret.is_none() {
                    self.fcx().frames.last_mut().unwrap().ret = exp_ret;
                }
                self.stmt(*b);
                let frame_ret = self.fcx().frames.last().unwrap().ret;
                match frame_ret {
                    Some(r) => {
                        if r != VOID && r != ERROR && !self.always_returns(*b) && !self.types.has_undefined(r) && !self.fcx.last().unwrap().reported_nonexhaustive {
                            self.report(Diagnostic::new("F0101", span, "arrow function doesn't return a value on every path"));
                        }
                        r
                    }
                    None => {
                        let mut all = std::mem::take(&mut self.fcx().frames.last_mut().unwrap().returns);
                        if all.is_empty() || all.iter().all(|&t| t == UNDEFINED || t == VOID) {
                            VOID
                        } else {
                            if !self.always_returns(*b) {
                                all.push(UNDEFINED);
                            }
                            self.types.union(&all)
                        }
                    }
                }
            }
        };
        let fcx = self.fcx.last_mut().unwrap();
        fcx.scopes.pop();
        fcx.frames.pop();
        self.types.func(params, body_ty)
    }

    fn new_expr(&mut self, callee: ExprId, type_args: &[crate::ast::TypeId], args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        let ExprKind::Ident(s) = self.ast().expr(callee).kind else { return ERROR };
        let name = self.name(s).to_string();
        let tscope = self.tscope();
        let targs: Vec<TyId> = type_args.iter().map(|&t| self.resolve_type(t, &tscope)).collect();
        let from_exp = |c: &mut Self, pick: &dyn Fn(&Ty) -> bool| exp.and_then(|e| c.flat_members(e).into_iter().find(|&m| pick(c.types.get(m))));
        match name.as_str() {
            "Map" => {
                for a in args {
                    self.expr(a.expr, None);
                }
                if !args.is_empty() {
                    self.report(Diagnostic::new("U0017", span, "`new Map(entries)` is not supported yet; create an empty map and `set` entries"));
                }
                if targs.len() == 2 {
                    return self.types.intern(Ty::Map(targs[0], targs[1]));
                }
                if let Some(t) = from_exp(self, &|t| matches!(t, Ty::Map(..))) {
                    return t;
                }
                self.report(Diagnostic::new("T0301", span, "can't infer the key and value types of this map").note("instead", "`new Map<string, int>()`, or annotate the binding"));
                ERROR
            }
            "Set" => {
                let mut elem = targs.first().copied().or_else(|| from_exp(self, &|t| matches!(t, Ty::Set(_))).and_then(|t| match self.types.get(t) {
                    Ty::Set(e) => Some(*e),
                    _ => None,
                }));
                if let Some(a) = args.first() {
                    let hint = elem.map(|e| self.types.array(e));
                    let t = self.expr(a.expr, hint);
                    match *self.types.get(t) {
                        Ty::Array(e) => elem = elem.or(Some(self.types.widen(e))),
                        Ty::Error => {}
                        _ => {
                            let msg = format!("`new Set(...)` takes an array, found `{}`", self.show(t));
                            let s = self.ast().expr(a.expr).span;
                            self.report(Diagnostic::new("T0001", s, msg));
                        }
                    }
                }
                match elem {
                    Some(e) => self.types.intern(Ty::Set(e)),
                    None => {
                        self.report(Diagnostic::new("T0301", span, "can't infer the element type of this set").note("instead", "`new Set<string>()`"));
                        ERROR
                    }
                }
            }
            "Array" => {
                self.report(Diagnostic::new("X0036", span, "`new Array(...)` is not supported").note("instead", "use an array literal: `const xs: T[] = []`"));
                ERROR
            }
            "Error" => {
                self.report(Diagnostic::new("U0004", span, "`Error` values are not supported yet (planned for M3)"));
                ERROR
            }
            _ => {
                self.report(Diagnostic::new("U0001", span, format!("`new {name}(...)`: classes are not supported yet (planned for M2)")).note("instead", "use a record type and a function that returns it"));
                ERROR
            }
        }
    }

    // ---------------------------------------------------------------- calls

    #[allow(clippy::too_many_arguments)]
    fn call(&mut self, e: ExprId, callee: ExprId, type_args: &[crate::ast::TypeId], args: &[Arg], optional: bool, exp: Option<TyId>, span: Span) -> TyId {
        let cnode = self.ast().expr(callee);
        match &cnode.kind {
            ExprKind::Ident(s) if self.lookup(*s).is_none() => {
                let text = self.name(*s).to_string();
                match self.scopes[self.cur as usize].values.get(s).map(|d| d.0) {
                    Some(Decl::Fn(m, i)) => {
                        let Some(sig) = self.fn_sig(m, i) else {
                            self.report(Diagnostic::new("T0303", cnode.span, format!("`{text}` is recursive, so it needs an explicit return type")));
                            for a in args {
                                self.expr(a.expr, None);
                            }
                            return ERROR;
                        };
                        let cs = CallSig { tparams: sig.tparams, params: sig.params, names: sig.param_names.iter().map(|&p| self.name(p).to_string()).collect(), rest: None, ret: sig.ret };
                        self.rec_call(e, Callee::Fn(m, i));
                        self.pending_call = Some(e);
                        return self.call_sig(&cs, &text, type_args, args, exp, span);
                    }
                    Some(_) => {}
                    None => {
                        self.rec_call(e, Callee::Builtin { ns: None, name: *s });
                        self.pending_call = Some(e);
                        if let Some(t) = self.global_call(&text, cnode.span, type_args, args, exp, span) {
                            return t;
                        }
                    }
                }
            }
            ExprKind::Member { obj, name, name_span, optional: mopt } => {
                let (obj, name, name_span, mopt) = (*obj, *name, *name_span, *mopt);
                let mtext = self.name(name).to_string();
                if let ExprKind::Ident(s) = self.ast().expr(obj).kind
                    && self.lookup(s).is_none() {
                        match self.scopes[self.cur as usize].values.get(&s).map(|d| d.0) {
                            Some(Decl::Ns(m)) => {
                                let scope = &self.scopes[m as usize];
                                if let Some(Decl::Fn(fm, fi)) = scope.values.get(&name).filter(|_| scope.exported_values.contains(&name)).map(|d| d.0) {
                                    let Some(sig) = self.fn_sig(fm, fi) else { return ERROR };
                                    let cs = CallSig { tparams: sig.tparams, params: sig.params, names: sig.param_names.iter().map(|&p| self.name(p).to_string()).collect(), rest: None, ret: sig.ret };
                                    let desc = format!("{}.{mtext}", self.name(s));
                                    self.rec_call(e, Callee::Fn(fm, fi));
                                    self.pending_call = Some(e);
                                    return self.call_sig(&cs, &desc, type_args, args, exp, span);
                                }
                            }
                            None => {
                                let ns = self.name(s).to_string();
                                if builtins::is_builtin_ns(&ns) {
                                    self.rec_call(e, Callee::Builtin { ns: Some(s), name });
                                    self.pending_call = Some(e);
                                    return self.builtin_ns_call(&ns, &mtext, name_span, args, exp, span);
                                }
                            }
                            _ => {}
                        }
                    }
                let recv = self.expr(obj, None);
                if recv == ERROR {
                    for a in args {
                        self.expr(a.expr, None);
                    }
                    return ERROR;
                }
                let base = if mopt {
                    self.types.without_undefined(recv)
                } else {
                    if self.types.has_undefined(recv) {
                        let d = self.possibly_undefined(obj, span);
                        self.report(d);
                    }
                    self.types.without_undefined(recv)
                };
                if let Ty::Expect(subject) = *self.types.get(base) {
                    self.rec_call(e, Callee::Matcher(subject));
                    self.pending_call = Some(e);
                    return self.expect_matcher(subject, &mtext, name_span, args, span);
                }
                // A record field holding a function.
                let is_field = self.flat_members(base).iter().all(|&m| self.field_of(m, name).is_some());
                let result = if is_field {
                    let ft = self.member_of(base, name, name_span, span, obj).unwrap_or(ERROR);
                    self.rec_call(e, Callee::Value);
                    self.pending_call = Some(e);
                    self.call_value(ft, &mtext, args, exp, span)
                } else if let Some(ms) = builtins::method(self, base, &mtext) {
                    if ms.mutates {
                        self.check_mutable_root_ext(obj, span, true);
                    }
                    let desc = format!("{}.{mtext}", builtins::type_label(&self.types, base));
                    self.rec_call(e, Callee::Method(base));
                    self.pending_call = Some(e);
                    self.call_sig(&ms.sig, &desc, type_args, args, exp, span)
                } else {
                    self.member_of(base, name, name_span, span, obj);
                    for a in args {
                        self.expr(a.expr, None);
                    }
                    ERROR
                };
                return if mopt || optional { self.types.optional(result) } else { result };
            }
            _ => {}
        }
        let t = self.expr(callee, None);
        let text = self.src(cnode.span).to_string();
        let t = if optional { self.types.without_undefined(t) } else { t };
        self.rec_call(e, Callee::Value);
        self.pending_call = Some(e);
        let r = self.call_value(t, &text, args, exp, span);
        if optional { self.types.optional(r) } else { r }
    }

    fn call_value(&mut self, t: TyId, desc: &str, args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        match *self.types.get(t) {
            Ty::Func(params, ret) => {
                let params = self.types.params(params).to_vec();
                let names = (0..params.len()).map(|i| format!("arg{i}")).collect();
                let cs = CallSig { tparams: Vec::new(), params, names, rest: None, ret };
                self.call_sig(&cs, desc, &[], args, exp, span)
            }
            Ty::Error => {
                for a in args {
                    self.expr(a.expr, None);
                }
                ERROR
            }
            _ => {
                for a in args {
                    self.expr(a.expr, None);
                }
                let msg = format!("`{desc}` is not a function (its type is `{}`)", self.show(t));
                self.report(Diagnostic::new("T0202", span, msg));
                ERROR
            }
        }
    }

    pub(super) fn describe_sig(&self, cs: &CallSig, desc: &str) -> String {
        let mut parts: Vec<String> = cs
            .params
            .iter()
            .zip(&cs.names)
            .map(|(p, n)| format!("{}{}{}: {}", if p.inout { "inout " } else { "" }, n, if p.optional { "?" } else { "" }, self.show(if p.optional { self.types_without_undef(p.ty) } else { p.ty })))
            .collect();
        if let Some(r) = cs.rest {
            parts.push(format!("...items: {}[]", self.show(r)));
        }
        format!("{desc}({}): {}", parts.join(", "), self.show(cs.ret))
    }

    fn types_without_undef(&self, t: TyId) -> TyId {
        match *self.types.get(t) {
            Ty::Union(ms) if self.types.tys(ms).len() == 2 && self.types.tys(ms).contains(&UNDEFINED) => *self.types.tys(ms).iter().find(|&&m| m != UNDEFINED).unwrap(),
            _ => t,
        }
    }

    pub(super) fn call_sig(&mut self, cs: &CallSig, desc: &str, type_args: &[crate::ast::TypeId], args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        let call_expr = self.pending_call.take();
        let required = cs.params.iter().filter(|p| !p.optional).count();
        let max = if cs.rest.is_some() { usize::MAX } else { cs.params.len() };
        if args.len() < required || args.len() > max {
            let expected = if cs.rest.is_some() {
                format!("at least {required}")
            } else if required == cs.params.len() {
                format!("{required}")
            } else {
                format!("{required} to {}", cs.params.len())
            };
            let d = Diagnostic::new("T0201", span, format!("`{desc}` takes {expected} argument{}, found {}", if expected == "1" { "" } else { "s" }, args.len()))
                .note("signature", self.describe_sig(cs, desc));
            self.report(d);
        }
        let mut map: HashMap<u32, TyId> = HashMap::default();
        if !type_args.is_empty() {
            if type_args.len() != cs.tparams.len() {
                let msg = format!("`{desc}` takes {} type argument(s), found {}", cs.tparams.len(), type_args.len());
                self.report(Diagnostic::new("T0010", span, msg));
            }
            let tscope = self.tscope();
            for (&p, &ta) in cs.tparams.iter().zip(type_args) {
                let t = self.resolve_type(ta, &tscope);
                map.insert(p, t);
            }
        }
        let saved_free = std::mem::take(&mut self.infer_free);
        self.infer_free = cs.tparams.iter().copied().filter(|p| !map.contains_key(p)).collect();
        let param_for = |i: usize| -> Option<FnParam> { cs.params.get(i).cloned().or_else(|| cs.rest.map(|ty| FnParam { ty, inout: false, optional: false })) };
        let mut arg_tys: Vec<Option<TyId>> = vec![None; args.len()];
        // Pass 1: non-closure arguments drive inference; pass 2: closures get the inferred parameter types.
        for pass in 0..2 {
            for (i, a) in args.iter().enumerate() {
                let is_arrow = matches!(self.ast().expr(a.expr).kind, ExprKind::Arrow(_));
                if (pass == 0) == is_arrow {
                    continue;
                }
                let Some(p) = param_for(i) else {
                    self.expr(a.expr, None);
                    continue;
                };
                let pty = self.types.subst(p.ty, &map);
                let still_free: Vec<u32> = self.infer_free.iter().copied().filter(|x| !map.contains_key(x)).collect();
                let hint = if is_arrow || !self.types.mentions(pty, &still_free) { Some(pty) } else { None };
                self.infer_free = still_free;
                let t = self.expr(a.expr, hint);
                arg_tys[i] = Some(t);
                let free = self.infer_free.clone();
                self.unify(p.ty, t, &free, &mut map);
            }
        }
        self.infer_free = saved_free;
        // Check each argument against the instantiated parameter type.
        let mut refs: Vec<(LocalId, Span)> = Vec::new();
        for (i, a) in args.iter().enumerate() {
            let Some(p) = param_for(i) else { continue };
            let Some(t) = arg_tys[i] else { continue };
            let pty = self.types.subst(p.ty, &map);
            let aspan = self.ast().expr(a.expr).span;
            let pname = cs.names.get(i).cloned().unwrap_or_else(|| "items".to_string());
            match (p.inout, a.by_ref) {
                (true, None) => {
                    let text = self.src(aspan).to_string();
                    self.report(
                        Diagnostic::new("V0110", aspan, format!("parameter `{pname}` of `{desc}` is `inout`; pass the argument with `&`"))
                            .fix(Applicability::Safe, format!("pass `&{text}`"), aspan.empty_at_start(), "&"),
                    );
                }
                (false, Some(amp)) => {
                    self.report(
                        Diagnostic::new("V0111", amp.to(aspan), format!("parameter `{pname}` of `{desc}` is not `inout`; pass the value without `&`"))
                            .fix(Applicability::Safe, "remove `&`", amp, ""),
                    );
                }
                (true, Some(_)) => {
                    self.invalidate_place(a.expr);
                    if let Some(place) = self.place(a.expr, true) {
                        if let Some(root) = place.root {
                            if let Some(&(_, prev)) = refs.iter().find(|(r, _)| *r == root) {
                                let n = self.name(self.local(root).name).to_string();
                                let _ = prev;
                                self.report(Diagnostic::new("V0113", aspan, format!("`{n}` is passed as `inout` more than once in this call")).note("why", "each `inout` argument must be a separate value"));
                            }
                            refs.push((root, aspan));
                            // The callee may change the value in place (for V0120).
                            if let Some(f) = self.fcx.last_mut() {
                                f.mutations.push((root, aspan));
                            }
                        }
                        if t != pty && t != ERROR && pty != ERROR {
                            let msg = format!("`inout` argument must be exactly `{}`, found `{}`", self.show(pty), self.show(t));
                            self.report(Diagnostic::new("T0001", aspan, msg));
                        }
                    }
                    continue;
                }
                (false, None) => {}
            }
            if !self.assignable(t, pty) {
                let d = self.mismatch(t, pty, aspan, Some(format!("argument `{pname}` of `{desc}`")));
                self.report(d);
            }
        }
        let unresolved: Vec<u32> = cs.tparams.iter().copied().filter(|p| !map.contains_key(p) && self.types.mentions(cs.ret, &[*p])).collect();
        if !unresolved.is_empty() {
            let names: Vec<String> = unresolved.iter().map(|&p| self.name(self.gparam(p).name).to_string()).collect();
            let _ = exp;
            self.report(
                Diagnostic::new("T0305", span, format!("can't infer type argument `{}` of `{desc}`", names.join("`, `")))
                    .note("instead", format!("pass it explicitly: `{desc}<{}>(...)`", names.join(", "))),
            );
            return ERROR;
        }
        let ret = self.types.subst(cs.ret, &map);
        if let Some(ce) = call_expr
            && self.facts.is_some()
        {
            let params: Vec<FnParam> = cs.params.iter().map(|p| FnParam { ty: self.types.subst(p.ty, &map), ..*p }).collect();
            let rest = cs.rest.map(|r| self.types.subst(r, &map));
            let targs: Vec<(u32, TyId)> = cs.tparams.iter().filter_map(|p| map.get(p).map(|t| (*p, *t))).collect();
            if let Some(f) = self.facts_mut()
                && let Some(c) = f.calls.get_mut(&ce)
            {
                c.params = params;
                c.rest = rest;
                c.targs = targs;
                c.ret = ret;
            }
        }
        ret
    }

    pub(super) fn unify(&mut self, pattern: TyId, actual: TyId, free: &[u32], map: &mut HashMap<u32, TyId>) {
        if actual == ERROR {
            return;
        }
        match *self.types.get(pattern) {
            Ty::Param(p) if free.contains(&p) => match map.get(&p).copied() {
                None => {
                    let w = self.types.widen(actual);
                    map.insert(p, w);
                }
                Some(bound) => {
                    if !self.assignable(actual, bound) && self.assignable(bound, actual) {
                        let w = self.types.widen(actual);
                        map.insert(p, w);
                    }
                }
            },
            Ty::Array(pe) => {
                if let Ty::Array(ae) = *self.types.get(actual) {
                    self.unify(pe, ae, free, map);
                }
            }
            Ty::Set(pe) => {
                if let Ty::Set(ae) = *self.types.get(actual) {
                    self.unify(pe, ae, free, map);
                }
            }
            Ty::Map(pk, pv) => {
                if let Ty::Map(ak, av) = *self.types.get(actual) {
                    self.unify(pk, ak, free, map);
                    self.unify(pv, av, free, map);
                }
            }
            Ty::Func(pps, pr) => {
                if let Ty::Func(aps, ar) = *self.types.get(actual) {
                    let (pps, aps) = (self.types.params(pps).to_vec(), self.types.params(aps).to_vec());
                    for (pp, ap) in pps.iter().zip(&aps) {
                        self.unify(pp.ty, ap.ty, free, map);
                    }
                    self.unify(pr, ar, free, map);
                }
            }
            Ty::Record(pfs) => {
                if let Ty::Record(afs) = *self.types.get(actual) {
                    let (pfs, afs) = (self.types.fields(pfs).to_vec(), self.types.fields(afs).to_vec());
                    for pf in &pfs {
                        if let Some(af) = afs.iter().find(|f| f.name == pf.name) {
                            self.unify(pf.ty, af.ty, free, map);
                        }
                    }
                }
            }
            Ty::Union(pms) => {
                let pattern_rest: Vec<TyId> = self.types.tys(pms).iter().copied().filter(|&m| m != UNDEFINED).collect();
                if pattern_rest.len() == 1 {
                    let actual_rest = self.types.without_undefined(actual);
                    self.unify(pattern_rest[0], actual_rest, free, map);
                }
            }
            Ty::Rec(pd, pargs) | Ty::Interface(pd, pargs) => {
                if let Ty::Rec(ad, aargs) | Ty::Interface(ad, aargs) = *self.types.get(actual)
                    && pd == ad {
                        let (pargs, aargs) = (self.types.tys(pargs).to_vec(), self.types.tys(aargs).to_vec());
                        for (p, a) in pargs.iter().zip(&aargs) {
                            self.unify(*p, *a, free, map);
                        }
                    }
            }
            _ => {}
        }
    }

    // ---------------------------------------------------------------- conditions and narrowing

    fn condition_ok(&mut self, t: TyId) -> bool {
        if t == BOOL || t == ERROR {
            return true;
        }
        if self.types.has_undefined(t) {
            let inner = self.types.without_undefined(t);
            return self.flat_members(inner).iter().all(|&m| !self.types.is_primitive(m) && m != BOOL);
        }
        false
    }

    pub(super) fn cond(&mut self, e: ExprId) {
        let t = self.expr(e, Some(BOOL));
        if self.condition_ok(t) {
            return;
        }
        let span = self.ast().expr(e).span;
        let text = self.src(span).to_string();
        let shown = self.show(t);
        let mut d = Diagnostic::new("T0401", span, format!("condition must be `bool`, found `{shown}`"))
            .note("why", "no implicit truthiness: `0`, `\"\"` and `undefined` are not `false`");
        let inner = self.types.without_undefined(t);
        if self.types.has_undefined(t) {
            d = d.fix(Applicability::Maybe, format!("test for presence: `{text} !== undefined`"), span, format!("{text} !== undefined"));
        } else if self.types.is_numeric(inner) {
            d = d.fix(Applicability::Maybe, format!("`{text} !== 0`"), span, format!("{text} !== 0"));
        } else if self.types.is_string(inner) {
            d = d.fix(Applicability::Maybe, format!("`{text} !== \"\"`"), span, format!("{text} !== \"\""));
        } else if matches!(self.types.get(inner), Ty::Array(_)) {
            d = d.fix(Applicability::Maybe, format!("`{text}.length > 0`"), span, format!("{text}.length > 0"));
        }
        self.report(d);
    }

    pub(super) fn apply(&mut self, ns: &[Narrowing]) {
        for n in ns {
            match n {
                Narrowing::Local(l, t) => self.narrow(*l, *t),
                Narrowing::Path(l, p, t) => self.narrow_path(*l, p.clone(), Some(*t)),
            }
        }
    }

    /// What we learn about locals when `e` evaluates to `positive`.
    pub(super) fn narrowings(&mut self, e: ExprId, positive: bool) -> Vec<Narrowing> {
        let ast = self.ast();
        match &ast.expr(e).kind {
            ExprKind::Paren(x) => self.narrowings(*x, positive),
            ExprKind::Unary(UnOp::Not, x) => self.narrowings(*x, !positive),
            ExprKind::Binary(BinOp::And, l, r) if positive => {
                let mut v = self.narrowings(*l, true);
                self.push_scope();
                self.apply(&v);
                v.extend(self.narrowings(*r, true));
                self.pop_scope();
                v
            }
            ExprKind::Binary(BinOp::Or, l, r) if !positive => {
                let mut v = self.narrowings(*l, false);
                self.push_scope();
                self.apply(&v);
                v.extend(self.narrowings(*r, false));
                self.pop_scope();
                v
            }
            ExprKind::Binary(op @ (BinOp::Eq | BinOp::Ne | BinOp::LooseEq | BinOp::LooseNe), l, r) => {
                let eq = matches!(op, BinOp::Eq | BinOp::LooseEq) == positive;
                let (l, r) = (*l, *r);
                let (subject, other) = if matches!(ast.expr(l).kind, ExprKind::Undefined | ExprKind::Str(_)) { (r, l) } else { (l, r) };
                match &ast.expr(other).kind {
                    ExprKind::Undefined => {
                        let Some((target, ty)) = self.target_of(subject) else { return Vec::new() };
                        let t = if eq { if self.types.has_undefined(ty) { UNDEFINED } else { NEVER } } else { self.types.without_undefined(ty) };
                        vec![target.with(t)]
                    }
                    ExprKind::Str(lit) => {
                        let lit = *lit;
                        let lit_ty = self.types.str_lit(lit);
                        match &ast.expr(subject).kind {
                            ExprKind::Member { obj, name, optional: false, .. } => {
                                let Some((target, ty)) = self.target_of(*obj) else { return Vec::new() };
                                let members = self.flat_members(ty);
                                let keep: Vec<TyId> = members
                                    .into_iter()
                                    .filter(|&m| match *self.types.get(m) {
                                        Ty::Record(fs) => match self.types.fields(fs).iter().find(|f| f.name == *name) {
                                            Some(f) if matches!(self.types.get(f.ty), Ty::StrLit(_)) => (f.ty == lit_ty) == eq,
                                            _ => true,
                                        },
                                        _ => true,
                                    })
                                    .collect();
                                vec![target.with(self.types.union(&keep))]
                            }
                            ExprKind::Typeof(x) => {
                                let Some((target, ty)) = self.target_of(*x) else { return Vec::new() };
                                let kind = self.name(lit).to_string();
                                let members = self.flat_members(ty);
                                let keep: Vec<TyId> = members.into_iter().filter(|&m| (typeof_name(&self.types, m) == Some(kind.as_str())) == eq).collect();
                                vec![target.with(self.types.union(&keep))]
                            }
                            _ => {
                                let Some((target, ty)) = self.target_of(subject) else { return Vec::new() };
                                let members = self.flat_members(ty);
                                if !members.iter().all(|&m| matches!(self.types.get(m), Ty::StrLit(_)) || m == UNDEFINED) {
                                    if eq {
                                        return vec![target.with(lit_ty)];
                                    }
                                    return Vec::new();
                                }
                                let keep: Vec<TyId> = members.into_iter().filter(|&m| (m == lit_ty) == eq).collect();
                                vec![target.with(self.types.union(&keep))]
                            }
                        }
                    }
                    _ => Vec::new(),
                }
            }
            ExprKind::Ident(_) | ExprKind::Member { optional: false, .. } => {
                let Some((target, ty)) = self.target_of(e) else { return Vec::new() };
                if !self.types.has_undefined(ty) {
                    return Vec::new();
                }
                let t = if positive { self.types.without_undefined(ty) } else { UNDEFINED };
                vec![target.with(t)]
            }
            _ => Vec::new(),
        }
    }

    /// A narrowable reference: a local, or a field path of a local (`t.left.value`), with its current type.
    fn target_of(&mut self, e: ExprId) -> Option<(Target, TyId)> {
        match &self.ast().expr(e).kind {
            ExprKind::Ident(s) => self.lookup(*s).map(|(id, ty)| (Target { local: id, path: Vec::new() }, ty)),
            ExprKind::Paren(x) => self.target_of(*x),
            ExprKind::Member { obj, name, optional: false, .. } => {
                let (mut target, base) = self.target_of(*obj)?;
                target.path.push(*name);
                if let Some(t) = self.lookup_path(target.local, &target.path) {
                    return Some((target, t));
                }
                let base = self.types.without_undefined(base);
                let members = self.flat_members(base);
                let mut tys = Vec::new();
                for m in members {
                    tys.push(self.field_of(m, *name)?);
                }
                let t = self.types.union(&tys);
                Some((target, t))
            }
            _ => None,
        }
    }

    /// Forgets narrowings at or under an assigned place (`t.left = x` invalidates `t.left.value`).
    pub(super) fn invalidate_place(&mut self, e: ExprId) {
        let mut path = Vec::new();
        let mut cur = e;
        loop {
            match &self.ast().expr(cur).kind {
                ExprKind::Paren(x) | ExprKind::NonNull(x) => cur = *x,
                ExprKind::Member { obj, name, .. } => {
                    path.push(*name);
                    cur = *obj;
                }
                ExprKind::Index { obj, .. } => {
                    // An element changed: forget everything under the collection's path.
                    path.clear();
                    cur = *obj;
                }
                ExprKind::Ident(s) => {
                    if let Some((id, _)) = self.lookup(*s) {
                        path.reverse();
                        self.narrow_path(id, path, None);
                    }
                    return;
                }
                _ => return,
            }
        }
    }
}

/// A reference that can be narrowed.
#[derive(Clone)]
pub(super) struct Target {
    local: LocalId,
    path: Vec<Sym>,
}

impl Target {
    fn with(self, ty: TyId) -> Narrowing {
        if self.path.is_empty() { Narrowing::Local(self.local, ty) } else { Narrowing::Path(self.local, self.path, ty) }
    }
}

pub(super) enum Narrowing {
    Local(LocalId, TyId),
    Path(LocalId, Vec<Sym>, TyId),
}

fn typeof_name(types: &Types, t: TyId) -> Option<&'static str> {
    if types.is_numeric(t) {
        Some("number")
    } else if types.is_string(t) {
        Some("string")
    } else if t == BOOL {
        Some("boolean")
    } else if t == UNDEFINED {
        Some("undefined")
    } else if matches!(types.get(t), Ty::Func(..)) {
        Some("function")
    } else {
        Some("object")
    }
}

fn int_range(t: TyId) -> Option<(i128, i128)> {
    Some(match t {
        INT => (i64::MIN as i128, i64::MAX as i128),
        I8 => (i8::MIN as i128, i8::MAX as i128),
        I16 => (i16::MIN as i128, i16::MAX as i128),
        I32 => (i32::MIN as i128, i32::MAX as i128),
        U8 => (0, u8::MAX as i128),
        U16 => (0, u16::MAX as i128),
        U32 => (0, u32::MAX as i128),
        U64 => (0, u64::MAX as i128),
        _ => return None,
    })
}
