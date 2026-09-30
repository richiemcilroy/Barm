//! Classes: object layout, construction, methods, dispatch, `instanceof`, printing and
//! reference counting.
//!
//! Layout: every object starts with a 16-byte header (`bmg_obj`: strong count, weak count,
//! class id, flags). A derived class embeds its base class's struct as its first member, so an
//! upcast is a pointer cast and C's aliasing rules hold. Each concrete instantiation of a class
//! (`Box<int>`, `Box<string>`) gets its own struct, class id, methods and drop function.
//!
//! Methods take the receiver as `self_`. Calls are direct unless a subclass overrides the
//! method; then they go through a dispatch function that switches on the class id (generated at
//! the end, when every instantiated class is known).

use super::body::FnBodyKind;
use super::{c_string, Gen, Val};
use crate::ast::{Arg, ExprId, ExprKind, MemberKind};
use crate::check::class::{CField, ClassMemberRef};
use crate::hash::FxMap;
use crate::intern::Sym;
use crate::source::Span;
use crate::types::*;
use std::fmt::Write;

#[derive(Clone)]
pub(crate) struct ClassInst {
    pub cid: u32,
    /// Struct name (without `*`).
    pub name: String,
    pub decl: u32,
    pub subst: FxMap<u32, TyId>,
    /// Some `new` creates instances of exactly this class.
    pub constructed: bool,
    /// A `cyclic class` (declared or inferred): tracked by the cycle collector.
    pub cyclic: bool,
}

pub(crate) enum ClassWork {
    Ctor { ty: TyId, cname: String },
    Method { ty: TyId, member: u32, cname: String, msubst: FxMap<u32, TyId> },
    Static { decl: u32, member: u32, cname: String, subst: FxMap<u32, TyId> },
}

pub(crate) struct Dispatcher {
    name: String,
    recv: TyId,
    method: Sym,
    params: Vec<FnParam>,
    ret: TyId,
}

/// C name of a class field.
pub(crate) fn field_cname(name: &str) -> String {
    format!("f_{}", name.replace('#', "P_"))
}

impl<'c, 'a> Gen<'c, 'a> {
    // ------------------------------------------------------------ instances and layout

    /// Registers a concrete class type (struct, class id).
    pub(crate) fn class_inst(&mut self, ty: TyId) -> ClassInst {
        if let Some(ci) = self.class_insts.get(&ty) {
            return ci.clone();
        }
        let Ty::Class(decl, args) = self.tget(ty) else { unreachable!("not a class type") };
        let args = self.c.types.tys(args).to_vec();
        self.c.resolve_class(decl);
        let subst: FxMap<u32, TyId> = self.c.classes[decl as usize].params.iter().copied().zip(args.iter().copied()).collect();
        let cname: String = self.c.class_names[decl as usize].chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
        let name = format!("K{}_{cname}", ty.0);
        let cid = self.class_list.len() as u32 + 1;
        let cyclic = self.c.classes[decl as usize].cyclic;
        let ci = ClassInst { cid, name, decl, subst, constructed: false, cyclic };
        self.class_insts.insert(ty, ci.clone());
        self.class_list.push(ty);
        ci
    }

    /// `struct K { base-or-header; own fields }` (called from `define_struct`).
    pub(crate) fn define_class_struct(&mut self, ty: TyId) -> String {
        let ci = self.class_inst(ty);
        let info = self.c.classes[ci.decl as usize].clone();
        let mut body = format!("struct {} {{\n", ci.name);
        match info.base {
            Some(b) => {
                let bt = self.c.types.subst(b, &ci.subst);
                let bn = self.box_name(bt);
                let _ = writeln!(body, "    {bn} base;");
            }
            None => body.push_str("    bmg_obj h;\n"),
        }
        for f in info.fields.iter().filter(|f| f.owner == ci.decl) {
            let fty = self.c.types.subst(f.ty, &ci.subst);
            let ct = self.ctype(fty);
            let _ = writeln!(body, "    {ct} {};", field_cname(self.sym(f.name)));
        }
        body.push_str("};\n");
        body
    }

    /// An lvalue for field `f` (instantiated for `cls`) of the object `obj`.
    fn field_lv_of(&mut self, obj: &str, cls: TyId, f: &CField) -> String {
        let fc = field_cname(self.sym(f.name));
        let owner = self.c.upcast_to(cls, f.owner).unwrap_or(cls);
        if owner == cls {
            format!("({obj})->{fc}")
        } else {
            let ot = self.ctype(owner);
            format!("(({ot})({obj}))->{fc}")
        }
    }

    pub(crate) fn class_field_lv_pub(&mut self, obj: &str, cls: TyId, f: &CField) -> String {
        self.field_lv_of(obj, cls, f)
    }

    /// A new instance of an `Error` class with `message` (C code of an owned string): returns the
    /// object pointer (owned, not a temporary).
    pub(crate) fn error_object(&mut self, cls: TyId, message: &str) -> String {
        self.ctype(cls);
        let ci = self.class_inst(cls);
        self.class_insts.get_mut(&cls).unwrap().constructed = true;
        let info = self.c.classes[ci.decl as usize].clone();
        let bn = ci.name.clone();
        let o = self.fresh("eo");
        self.line(format!("{bn} *{o} = bmg_alloc_small(sizeof({bn})); bmg_obj_init({o}, {});", ci.cid));
        for f in &info.fields {
            let fty = self.c.types.subst(f.ty, &ci.subst);
            let d = self.default_value(fty);
            let lv = self.field_lv_of(&o, cls, &CField { ty: fty, ..f.clone() });
            self.line(format!("{lv} = {d};"));
        }
        let ctor = self.ctor_instance(cls);
        let mv = self.fresh("em");
        self.line(format!("bm_str {mv} = {message};"));
        let mut args = vec![o.clone()];
        let mut after = Vec::new();
        if let Some(p) = info.ctor.params.first() {
            let pty = self.c.types.subst(p.ty, &ci.subst);
            let v = self.coerce(Val::plain(mv.clone(), STR), pty);
            // Owned here (not a statement temporary: this code sits inside a branch).
            let code = self.consume(v);
            after.push(format!("{};", self.release_code(pty, &code)));
            args.push(code);
        }
        self.line(format!("{ctor}({});", args.join(", ")));
        for a in after {
            self.line(a);
        }
        self.line(format!("bm_str_release({mv});"));
        o
    }

    /// The field named `name` of class type `cls`, instantiated.
    fn class_field(&mut self, cls: TyId, name: Sym) -> Option<CField> {
        match self.c.class_member(cls, name)? {
            ClassMemberRef::Field(f) => Some(f),
            _ => None,
        }
    }

    // ------------------------------------------------------------ field access

    /// `base.name` on a class instance: a field read (borrowed) or a getter call.
    pub(crate) fn class_field_read(&mut self, base: Val, name: Sym, ty: TyId, span: Span) -> Val {
        match self.c.class_member(base.ty, name) {
            Some(ClassMemberRef::Field(f)) => {
                let lv = self.field_lv_of(&base.code, base.ty, &f);
                let v = if f.weak { self.weak_load(&lv, f.ty) } else { Val { code: lv, ty: f.ty, owned: false } };
                let v = if self.c.assignable_pub(ty, f.ty) && ty != f.ty && !self.c.assignable_pub(f.ty, ty) { self.project(v, ty) } else { self.coerce(v, ty) };
                if base.owned && !v.owned && self.is_rc(v.ty) {
                    return self.own(v);
                }
                v
            }
            Some(ClassMemberRef::Getter(g)) => {
                let ret = self.c.method_ret(&g);
                let code = self.method_call_code(&base.code, base.ty, name, false, Vec::new());
                let v = if ret == VOID { Val::plain("0", VOID) } else { self.tmp(ret, &code, true) };
                self.coerce(v, ty)
            }
            _ => {
                self.unsupported(span, "this class member access");
                Val::plain("0", ty)
            }
        }
    }

    /// Reads a weak field (`C | undefined` union): `undefined` once the target is freed.
    fn weak_load(&mut self, lv: &str, uty: TyId) -> Val {
        let ct = self.ctype(uty);
        let v = self.fresh("w");
        self.line(format!("{ct} {v} = {lv};"));
        if let (Some(k), Some(u)) = (self.class_member_tag(uty), self.tag_of(uty, UNDEFINED)) {
            let (is, pl, und) = (self.u_is(&v, uty, k), self.u_payload(&v, uty, k), self.u_make(uty, u, None));
            self.line(format!("if ({is} && ((bmg_obj *){pl})->rc <= 0) {v} = {und};"));
        }
        Val::plain(v, uty)
    }

    fn class_member_tag(&self, uty: TyId) -> Option<usize> {
        self.members(uty).iter().position(|&m| matches!(self.tget(m), Ty::Class(..)))
    }

    /// An lvalue for `obj.name = ...` on a class instance (the object is kept alive for the statement).
    pub(crate) fn class_field_place(&mut self, obj: ExprId, name: Sym, span: Span) -> super::body::Place {
        let v = self.expr(obj);
        let inner = self.c.types.without_undefined(v.ty);
        let v = self.project(v, inner);
        let v = if self.heap_rooted(obj) { self.own(v) } else { v };
        let Some(f) = self.class_field(v.ty, name) else {
            self.unsupported(span, "assigning to this class member");
            return super::body::Place { lv: "bm__dummy".into(), ty: ERROR, weak: false };
        };
        let lv = self.field_lv_of(&v.code, v.ty, &f);
        super::body::Place { lv, ty: f.ty, weak: f.weak }
    }

    /// `field = value` for a weak field: weak counts instead of strong ones.
    pub(crate) fn weak_store(&mut self, lv: &str, uty: TyId, v: Val) -> Val {
        let v = self.coerce(v, uty);
        let ct = self.ctype(uty);
        let (nv, pv) = (self.fresh("n"), self.fresh("p"));
        self.line(format!("{ct} {nv} = {}; {ct} *{pv} = &{lv};", v.code));
        if let Some(k) = self.class_member_tag(uty) {
            let (nis, npl) = (self.u_is(&nv, uty, k), self.u_payload(&nv, uty, k));
            let old = format!("(*{pv})");
            let (ois, opl) = (self.u_is(&old, uty, k), self.u_payload(&old, uty, k));
            self.line(format!("if ({nis}) bmg_weak_retain({npl});"));
            self.line(format!("if ({ois}) bmg_weak_release({opl});"));
        }
        self.line(format!("*{pv} = {nv};"));
        Val::plain(format!("(*{pv})"), uty)
    }

    /// Is the value of `e` reached through a class instance's field (another reference could
    /// release it while we use it)? `this.f` chains through `readonly` fields are stable.
    pub(crate) fn heap_rooted(&mut self, e: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        match &ast.expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) => self.heap_rooted(*x),
            ExprKind::Member { obj, name, .. } => {
                let ot = self.ty(*obj);
                let ot = self.c.types.without_undefined(ot);
                if self.c.class_of(ot).is_some() {
                    let readonly = matches!(self.class_field(ot, *name), Some(f) if f.readonly && !f.weak);
                    let stable_obj = matches!(ast.expr(*obj).kind, ExprKind::This) || self.stable_local(*obj) || !self.heap_rooted(*obj) && matches!(ast.expr(*obj).kind, ExprKind::Member { .. });
                    return !(readonly && stable_obj);
                }
                self.heap_rooted(*obj)
            }
            ExprKind::Index { obj, .. } => self.heap_rooted(*obj) || self.boxed_local(*obj),
            ExprKind::Ident(_) => match self.ident_fact(m, e) {
                // A module variable that code changes: a call may replace it while we borrow it.
                Some(crate::check::IdentFact::Const(cm, ci)) => self.mutated_globals.contains(&(cm, ci)),
                _ => self.boxed_local(e),
            },
            _ => false,
        }
    }

    fn stable_local(&self, e: ExprId) -> bool {
        let m = self.cur_m();
        match self.ident_fact(m, e) {
            Some(crate::check::IdentFact::Local(k)) => {
                let b = self.bodies.last().unwrap();
                !b.mutated.contains(&k) && !b.boxed.contains(&k)
            }
            _ => false,
        }
    }

    fn boxed_local(&self, e: ExprId) -> bool {
        let m = self.cur_m();
        match (&self.ast(m).expr(e).kind, self.ident_fact(m, e)) {
            (ExprKind::Ident(_), Some(crate::check::IdentFact::Local(k))) => self.bodies.last().unwrap().boxed.contains(&k),
            _ => false,
        }
    }

    // ------------------------------------------------------------ construction

    /// `new C(args)`: allocate, set every field to a default, run the constructor.
    pub(crate) fn new_object(&mut self, _e: ExprId, ty: TyId, args: &[Arg], params: &[FnParam], _span: Span) -> Val {
        let moves = self.ctor_moves(ty);
        let argv = self.ctor_args(args, params, &moves);
        self.ctype(ty);
        let mut ci = self.class_inst(ty);
        if !ci.constructed {
            self.class_insts.get_mut(&ty).unwrap().constructed = true;
            ci.constructed = true;
        }
        let bn = ci.name.clone();
        let o = self.fresh("o");
        if ci.cyclic {
            self.line(format!("{bn} *{o} = bmg_cyc_alloc(sizeof({bn})); bmg_obj_init({o}, {}); ((bmg_obj *){o})->flags = BMG_CYCLIC;", ci.cid));
        } else {
            self.line(format!("{bn} *{o} = bmg_alloc_small(sizeof({bn})); bmg_obj_init({o}, {});", ci.cid));
        }
        let info = self.c.classes[ci.decl as usize].clone();
        for f in &info.fields {
            let fty = self.c.types.subst(f.ty, &ci.subst);
            let d = self.default_value(fty);
            let lv = self.field_lv_of(&o, ty, &CField { ty: fty, ..f.clone() });
            self.line(format!("{lv} = {d};"));
        }
        let ctor = self.ctor_instance(ty);
        let mut all = vec![o.clone()];
        all.extend(argv);
        self.line(format!("{ctor}({});", all.join(", ")));
        self.b().temps.last_mut().unwrap().push((o.clone(), ty));
        Val { code: o, ty, owned: true }
    }

    fn ctor_instance(&mut self, ty: TyId) -> String {
        if let Some(n) = self.ctor_insts.get(&ty) {
            return n.clone();
        }
        let ci = self.class_inst(ty);
        let cname = format!("ctor_{}", ci.name);
        self.ctor_insts.insert(ty, cname.clone());
        let info = self.c.classes[ci.decl as usize].clone();
        let params: Vec<FnParam> = info.ctor.params.iter().map(|p| FnParam { ty: self.c.types.subst(p.ty, &ci.subst), ..*p }).collect();
        let proto = self.self_proto(&cname, &ci.name, &params, VOID);
        let _ = writeln!(self.protos, "static {proto};");
        self.class_work.push(ClassWork::Ctor { ty, cname: cname.clone() });
        cname
    }

    fn self_proto(&mut self, cname: &str, self_struct: &str, params: &[FnParam], ret: TyId) -> String {
        let rct = if ret == VOID { "void".to_string() } else { self.ctype(ret) };
        let mut ps = vec![format!("{self_struct} *self_")];
        for (i, p) in params.iter().enumerate() {
            let ct = self.ctype(p.ty);
            ps.push(if p.inout { format!("{ct} *p{i}") } else { format!("{ct} p{i}") });
        }
        format!("{rct} {cname}({})", ps.join(", "))
    }

    /// Which constructor parameters are moved into the object (parameter properties the
    /// constructor never reassigns): the caller hands over ownership instead of lending them.
    pub(crate) fn ctor_moves(&mut self, ty: TyId) -> Vec<bool> {
        let Some((decl, _)) = self.c.class_of(ty) else { return Vec::new() };
        if let Some(v) = self.ctor_move_memo.get(&decl) {
            return v.clone();
        }
        let info = self.c.classes[decl as usize].clone();
        let v = match info.ctor.member {
            Some(mi) => {
                let fd = self.c.member_fn(decl, mi);
                let ast = self.ast(info.module);
                let mut exprs = Vec::new();
                super::body::collect_exprs_stmt_pub(ast, fd.body, &mut exprs);
                fd.params
                    .iter()
                    .map(|p| {
                        p.prop.is_some()
                            && !exprs.iter().any(|&e| match &ast.expr(e).kind {
                                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => match &ast.expr(*t).kind {
                                    ExprKind::Ident(s) => *s == p.name,
                                    ExprKind::Member { obj, name, .. } => *name == p.name && matches!(ast.expr(*obj).kind, ExprKind::This),
                                    _ => false,
                                },
                                ExprKind::Call { args, .. } => args.iter().any(|a| a.by_ref.is_some() && matches!(ast.expr(a.expr).kind, ExprKind::Ident(s) if s == p.name)),
                                _ => false,
                            })
                    })
                    .collect()
            }
            None => match info.base {
                Some(b) => {
                    let (bd, bargs) = self.c.class_of(b).unwrap();
                    let bt = {
                        let ci = self.class_inst(ty);
                        let bt = self.c.types.subst(b, &ci.subst);
                        let _ = (bd, bargs);
                        bt
                    };
                    self.ctor_moves(bt)
                }
                None => Vec::new(),
            },
        };
        self.ctor_move_memo.insert(decl, v.clone());
        v
    }

    /// Constructor arguments: moved ones are handed over (owned), the rest lent as usual.
    fn ctor_args(&mut self, args: &[Arg], params: &[FnParam], moves: &[bool]) -> Vec<String> {
        if !moves.iter().any(|&m| m) {
            return self.args_pub(args, params);
        }
        let mut out = Vec::new();
        for (i, a) in args.iter().enumerate() {
            let p = params.get(i).copied().unwrap_or(FnParam { ty: ERROR, inout: false, optional: false });
            if moves.get(i).copied().unwrap_or(false) {
                let v = self.expr(a.expr);
                let v = self.coerce(v, p.ty);
                out.push(self.consume(v));
            } else {
                let one = self.args_pub(std::slice::from_ref(a), std::slice::from_ref(&p));
                out.extend(one);
            }
        }
        for (i, p) in params.iter().enumerate().skip(args.len()) {
            let v = self.coerce(Val::plain("0", UNDEFINED), p.ty);
            let code = if moves.get(i).copied().unwrap_or(false) { self.consume(v) } else { v.code };
            out.push(code);
        }
        out
    }

    /// `super(args)`: the base constructor on `this`, then this class's field initializers.
    pub(crate) fn super_ctor_call(&mut self, base: TyId, args: &[Arg], params: &[FnParam]) {
        let moves = self.ctor_moves(base);
        let argv = self.ctor_args(args, params, &moves);
        let ctor = self.ctor_instance(base);
        let bn = self.box_name(base);
        let mut all = vec![format!("({bn} *)self_")];
        all.extend(argv);
        self.line(format!("{ctor}({});", all.join(", ")));
        if let Some(cls) = self.b().ctor_class.take() {
            self.emit_field_inits(cls);
        }
    }

    /// Parameter properties and field initializers of the class itself (not its bases).
    pub(crate) fn emit_field_inits(&mut self, cls: TyId) {
        let ci = self.class_inst(cls);
        let info = self.c.classes[ci.decl as usize].clone();
        let cd = self.c.class_decl(ci.decl);
        let ctor_fd = info.ctor.member.map(|mi| self.c.member_fn(ci.decl, mi));
        let moves = self.ctor_moves(cls);
        for f in info.fields.iter().filter(|f| f.owner == ci.decl) {
            let fty = self.c.types.subst(f.ty, &ci.subst);
            let inst_f = CField { ty: fty, ..f.clone() };
            self.push_temps_pub();
            let v = if let Some(pi) = f.prop_param {
                let Some(fd) = ctor_fd else { self.pop_temps_pub(); continue };
                let key = fd.params[pi as usize].span.start;
                let Some(local) = self.b().locals.get(&key).cloned() else { self.pop_temps_pub(); continue };
                if moves.get(pi as usize).copied().unwrap_or(false) && !f.weak {
                    // Moved in by the caller: the field takes the reference as is.
                    let lv = self.field_lv_of("self_", cls, &inst_f);
                    let r = self.release_code(fty, &lv);
                    self.line(format!("{r}; {lv} = {};", local.access));
                    self.pop_temps_pub();
                    continue;
                }
                self.project(Val::plain(local.access, local.ty), fty)
            } else if let Some(mi) = f.member {
                let mem = &cd.members[mi as usize];
                let MemberKind::Field { init: Some(e), .. } = &mem.kind else { self.pop_temps_pub(); continue };
                // `this` inside the initializer.
                self.b().locals.insert(mem.name_span.start, super::body::Local { access: "self_".into(), ty: cls });
                let m = self.cur_m();
                let saved_m = self.b().m;
                let decl_m = self.c.classes[ci.decl as usize].module;
                self.b().m = decl_m;
                let v = self.expr(*e);
                self.b().m = saved_m;
                let _ = m;
                v
            } else {
                self.pop_temps_pub();
                continue;
            };
            let lv = self.field_lv_of("self_", cls, &inst_f);
            if f.weak {
                self.weak_store(&lv, fty, v);
            } else {
                let v = self.coerce(v, fty);
                let code = self.consume(v);
                let ct = self.ctype(fty);
                let nv = self.fresh("n");
                self.line(format!("{ct} {nv} = {code};"));
                let r = self.release_code(fty, &lv);
                self.line(format!("{r}; {lv} = {nv};"));
            }
            self.pop_temps_pub();
        }
    }

    // ------------------------------------------------------------ methods

    /// C name of method `member` of the class instance `owner_ty` (queued for emission).
    /// A method's C function; `msubst` instantiates a generic method's own type parameters.
    fn method_instance(&mut self, owner_ty: TyId, member: u32, msubst: &FxMap<u32, TyId>) -> String {
        let mut targs: Vec<(u32, TyId)> = msubst.iter().map(|(&k, &v)| (k, v)).collect();
        targs.sort();
        if targs.is_empty() {
            if let Some(n) = self.method_insts.get(&(owner_ty, member)) {
                return n.clone();
            }
        } else if let Some(n) = self.generic_method_insts.get(&(owner_ty, member, targs.clone())) {
            return n.clone();
        }
        let ci = self.class_inst(owner_ty);
        let mut subst = ci.subst.clone();
        subst.extend(msubst.iter().map(|(&k, &v)| (k, v)));
        let (params, ret) = self.own_method_sig(ci.decl, member, &subst);
        let mname = self.sym(self.c.class_member_decl(ci.decl, member).name).replace('#', "P_");
        let cname = if targs.is_empty() {
            let n = format!("M{}_{mname}", ci.name);
            self.method_insts.insert((owner_ty, member), n.clone());
            n
        } else {
            let n = format!("M{}_{mname}_{}", ci.name, self.generic_method_insts.len());
            self.generic_method_insts.insert((owner_ty, member, targs), n.clone());
            n
        };
        let proto = self.self_proto(&cname, &ci.name, &params, ret);
        let _ = writeln!(self.protos, "static {proto};");
        self.class_work.push(ClassWork::Method { ty: owner_ty, member, cname: cname.clone(), msubst: msubst.clone() });
        cname
    }

    /// Parameters and return type of a class's own method, instantiated.
    fn own_method_sig(&mut self, decl: u32, member: u32, subst: &FxMap<u32, TyId>) -> (Vec<FnParam>, TyId) {
        let info = self.c.classes[decl as usize].clone();
        let mm = info.methods.iter().chain(info.statics.iter()).find(|m| m.owner == decl && m.member == member).cloned();
        let Some(mm) = mm else { return (Vec::new(), VOID) };
        let ret = self.c.method_ret(&mm);
        let params = mm.params.iter().map(|p| FnParam { ty: self.c.types.subst(p.ty, subst), ..*p }).collect();
        (params, self.c.types.subst(ret, subst))
    }

    /// A static method's C function (one per type-argument list for a generic one).
    pub(crate) fn static_method_instance(&mut self, decl: u32, member: u32, subst: FxMap<u32, TyId>) -> String {
        let mut targs: Vec<(u32, TyId)> = subst.iter().map(|(&k, &v)| (k, v)).collect();
        targs.sort();
        let key = (decl, member, targs);
        if let Some(n) = self.static_insts.get(&key) {
            return n.clone();
        }
        let (params, ret) = self.own_method_sig(decl, member, &subst);
        let cls: String = self.c.class_names[decl as usize].chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
        let mname = self.sym(self.c.class_member_decl(decl, member).name).replace('#', "P_");
        let cname = if subst.is_empty() { format!("S{decl}_{cls}_{mname}") } else { format!("S{decl}_{cls}_{mname}_{}", self.static_insts.len()) };
        self.static_insts.insert(key, cname.clone());
        let proto = self.fn_proto(&cname, &params, ret, &FxMap::default(), false);
        let _ = writeln!(self.protos, "static {proto};");
        self.class_work.push(ClassWork::Static { decl, member, cname: cname.clone(), subst });
        cname
    }

    /// Is method `name` (as resolved on `decl`) overridden in some subclass declaration?
    fn is_overridden(&mut self, decl: u32, name: Sym) -> bool {
        if let Some(&v) = self.overridden.get(&(decl, name)) {
            return v;
        }
        let base = self.c.classes[decl as usize].methods.iter().find(|m| m.name == name).map(|m| (m.owner, m.member));
        let mut v = false;
        for d in 0..self.c.classes.len() as u32 {
            if d == decl || !self.c.class_descends(d, decl) {
                continue;
            }
            self.c.resolve_class(d);
            let here = self.c.classes[d as usize].methods.iter().find(|m| m.name == name).map(|m| (m.owner, m.member));
            if here != base {
                v = true;
                break;
            }
        }
        self.overridden.insert((decl, name), v);
        v
    }

    /// Code calling method `name` on receiver `recv` (of class type `recv_ty`) with `argv`.
    pub(crate) fn method_call_code(&mut self, recv: &str, recv_ty: TyId, name: Sym, sup: bool, argv: Vec<String>) -> String {
        self.method_call_code_with(recv, recv_ty, name, sup, argv, &FxMap::default())
    }

    /// `msubst`: a generic method's type arguments at this call.
    pub(crate) fn method_call_code_with(&mut self, recv: &str, recv_ty: TyId, name: Sym, sup: bool, argv: Vec<String>, msubst: &FxMap<u32, TyId>) -> String {
        let Some(ClassMemberRef::Method(mm) | ClassMemberRef::Getter(mm)) = self.c.class_member(recv_ty, name) else { return "0".into() };
        let Some((decl, _)) = self.c.class_of(recv_ty) else { return "0".into() };
        let mut all = Vec::new();
        let f = if !sup && self.is_overridden(decl, name) {
            all.push(recv.to_string());
            self.dispatcher(recv_ty, name, &mm)
        } else {
            let owner = self.c.upcast_to(recv_ty, mm.owner).unwrap_or(recv_ty);
            let ot = self.ctype(owner);
            all.push(if owner == recv_ty { recv.to_string() } else { format!("({ot})({recv})") });
            self.method_instance(owner, mm.member, msubst)
        };
        all.extend(argv);
        format!("{f}({})", all.join(", "))
    }

    fn dispatcher(&mut self, recv_ty: TyId, name: Sym, mm: &crate::check::class::CMethod) -> String {
        if let Some(n) = self.dispatch_names.get(&(recv_ty, name)) {
            return n.clone();
        }
        let ci = self.class_inst(recv_ty);
        let mname = self.sym(name).replace('#', "P_");
        let dname = format!("vd_{}_{mname}", ci.name);
        let ret = self.c.method_ret(mm);
        let proto = self.self_proto(&dname, &ci.name, &mm.params, ret);
        let _ = writeln!(self.protos, "static {proto};");
        self.dispatch_names.insert((recv_ty, name), dname.clone());
        self.dispatchers.push(Dispatcher { name: dname.clone(), recv: recv_ty, method: name, params: mm.params.clone(), ret });
        dname
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn class_method_call(&mut self, e: ExprId, recv: TyId, name: Sym, sup: bool, args: &[Arg], params: &[FnParam], ty: TyId, span: Span) -> Val {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Call { callee, .. } = &ast.expr(e).kind else { return Val::plain("0", ty) };
        let ExprKind::Member { obj, optional: mopt, .. } = &ast.expr(*callee).kind else { return Val::plain("0", ty) };
        // A generic method's type arguments at this call.
        let mut msubst = FxMap::default();
        if let Some(fact) = self.facts(m).calls.get(&e).cloned() {
            for (p, t) in &fact.targs {
                let t = self.inst(*t);
                msubst.insert(*p, t);
            }
        }
        let ret = match self.c.class_member(recv, name) {
            Some(ClassMemberRef::Method(mm)) => self.c.method_ret(&mm),
            _ => {
                self.unsupported(span, "this method call");
                return Val::plain("0", ty);
            }
        };
        let ret = self.c.types.subst(ret, &msubst);
        if sup {
            let bn = self.ctype(recv);
            let argv = self.args_pub(args, params);
            let code = self.method_call_code_with(&format!("({bn})self_"), recv, name, true, argv, &msubst);
            return self.finish_call_pub(&code, ret, ty);
        }
        let v = self.expr(*obj);
        if *mopt && self.c.types.has_undefined(v.ty) {
            // x?.m(): undefined when x is undefined.
            let present = self.truthy_val(v.clone());
            let res = self.fresh("t");
            let ct = self.ctype(ty);
            self.line(format!("{ct} {res};"));
            self.open_block(&format!("if ({present}) {{"));
            let rv = self.project(v, recv);
            let argv = self.args_pub(args, params);
            let code = self.method_call_code_with(&rv.code, recv, name, false, argv, &msubst);
            let inner = self.c.types.without_undefined(ty);
            let r = self.finish_call_pub(&code, ret, inner);
            let r = self.coerce(r, ty);
            let rc = self.consume(r);
            self.line(format!("{res} = {rc};"));
            self.mid_block("} else {");
            let u = self.coerce(Val::plain("0", UNDEFINED), ty);
            self.line(format!("{res} = {};", u.code));
            self.close_block("}");
            let owned = self.is_rc(ty);
            if owned {
                self.b().temps.last_mut().unwrap().push((res.clone(), ty));
            }
            return Val { code: res, ty, owned };
        }
        let v = self.project(v, recv);
        let v = if self.heap_rooted(*obj) { self.own(v) } else { v };
        let argv = self.args_pub(args, params);
        let code = self.method_call_code_with(&v.code, recv, name, false, argv, &msubst);
        self.finish_call_pub(&code, ret, ty)
    }

    // ------------------------------------------------------------ statics

    pub(crate) fn static_field_ref(&mut self, decl: u32, member: u32) -> Val {
        let info = self.c.classes[decl as usize].clone();
        let Some(f) = info.static_fields.iter().find(|f| f.member == Some(member)).cloned() else { return Val::plain("0", ERROR) };
        let cls: String = self.c.class_names[decl as usize].chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
        let gname = format!("gs{decl}_{cls}_{}", self.sym(f.name));
        if self.statics_done.insert((decl, member)) {
            let ct = self.ctype(f.ty);
            let _ = writeln!(self.protos, "static {ct} {gname};");
            let MemberKind::Field { init: Some(init), .. } = &self.c.class_member_decl(decl, member).kind else { return Val::plain(gname, f.ty) };
            let m = info.module;
            let code = self.function_body(m, FxMap::default(), &[], f.ty, FnBodyKind::Expr(*init), None, None, None);
            let fname = format!("init_{gname}");
            let _ = writeln!(self.funcs, "static {ct} {fname}(void) {{\n{code}}}\n");
            let _ = writeln!(self.const_init, "    {gname} = {fname}();");
        }
        Val::plain(gname, f.ty)
    }

    // ------------------------------------------------------------ conversions

    /// Upcast (always valid) or downcast (after a check) between class types: a pointer cast.
    pub(crate) fn class_cast(&mut self, v: Val, to: TyId) -> Val {
        let ct = self.ctype(to);
        if v.owned {
            let code = self.consume(v);
            return self.tmp(to, &format!("(({ct})({code}))"), true);
        }
        Val::plain(format!("(({ct})({}))", v.code), to)
    }

    /// `v instanceof C` as a C boolean.
    pub(crate) fn instanceof_code(&mut self, v: Val, decl: u32) -> String {
        let isa = self.isa_fn(decl);
        match self.tget(v.ty) {
            Ty::Class(d, _) => {
                if self.c.class_descends(d, decl) {
                    "true".into()
                } else {
                    format!("{isa}(((bmg_obj *)({}))->cid)", v.code)
                }
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let mut parts = Vec::new();
                for (i, &m) in ms.iter().enumerate() {
                    if let Ty::Class(d, _) = self.tget(m) {
                        let (is, pl) = (self.u_is(&v.code, v.ty, i), self.u_payload(&v.code, v.ty, i));
                        if self.c.class_descends(d, decl) {
                            parts.push(is);
                        } else if self.c.class_descends(decl, d) {
                            parts.push(format!("({is} && {isa}(((bmg_obj *){pl})->cid))"));
                        }
                    }
                }
                if parts.is_empty() { "false".into() } else { format!("({})", parts.join(" || ")) }
            }
            _ => "false".into(),
        }
    }

    fn isa_fn(&mut self, decl: u32) -> String {
        let name = format!("bmg_isa_{decl}");
        if self.isa_decls.insert(decl) {
            let _ = writeln!(self.protos, "static bool {name}(uint32_t cid);");
        }
        name
    }

    /// Releases a class reference. When no other class derives from it, the object's class is
    /// known: call its drop function directly (the C compiler can inline it) instead of
    /// dispatching on the class id.
    pub(crate) fn class_release_code(&mut self, ty: TyId, place: &str) -> String {
        let Some((decl, _)) = self.c.class_of(ty) else { return format!("bmg_obj_release({place})") };
        let has_subclass = (0..self.c.classes.len() as u32).any(|d| d != decl && self.c.class_descends(d, decl));
        if has_subclass {
            return format!("bmg_obj_release({place})");
        }
        let ci = self.class_inst(ty);
        let name = format!("rl_{}", ci.name);
        if self.helpers_done.insert((ty, 101)) {
            let _ = writeln!(self.protos, "static void drop_{}(void *p);", ci.name);
            let _ = writeln!(self.helpers, "static inline void {name}(void *p) {{ if (p && --((bmg_obj *)p)->rc == 0) drop_{}(p); }}", ci.name);
        }
        format!("{name}({place})")
    }

    /// Can some `weak` field point to an instance of class `decl`?
    fn weak_target(&mut self, decl: u32) -> bool {
        if let Some(&v) = self.weak_memo.get(&decl) {
            return v;
        }
        let mut v = false;
        'outer: for c in 0..self.c.classes.len() as u32 {
            self.c.resolve_class(c);
            let fields = self.c.classes[c as usize].fields.clone();
            for f in fields.iter().filter(|f| f.weak && f.owner == c) {
                for m in self.c.types.members(f.ty) {
                    if let Some((wd, _)) = self.c.class_of(m)
                        && (self.c.class_descends(decl, wd) || self.c.class_descends(wd, decl))
                    {
                        v = true;
                        break 'outer;
                    }
                }
            }
        }
        self.weak_memo.insert(decl, v);
        v
    }

    // ------------------------------------------------------------ cycle collection

    /// Can a value of class `decl` (or a subclass) be a cyclic object?
    fn maybe_cyclic(&mut self, decl: u32) -> bool {
        (0..self.c.classes.len() as u32).any(|d| self.c.classes[d as usize].cyclic && self.c.class_descends(d, decl))
    }

    /// Code visiting the cyclic-object references inside a value at `place` (None: there are none).
    fn gc_visit(&mut self, ty: TyId, place: &str, gv: &mut FxMap<TyId, String>) -> Option<String> {
        match self.tget(ty) {
            Ty::Class(d, _) => self.maybe_cyclic(d).then(|| format!("bmg_gc_obj({place});")),
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                if let Some((pi, _)) = self.niche(ty) {
                    return self.gc_visit(ms[pi], place, gv);
                }
                let mut cases = String::new();
                for (i, &m) in ms.iter().enumerate() {
                    if let Some(code) = self.gc_visit(m, &format!("({place}).u.m{i}"), gv) {
                        let _ = write!(cases, " case {i}: {code} break;");
                    }
                }
                (!cases.is_empty()).then(|| format!("switch (({place}).tag) {{{cases} default: break; }}"))
            }
            Ty::Record(fs) => {
                let fs = self.c.types.fields(fs).to_vec();
                let mut out = String::new();
                for f in fs {
                    if let Some(code) = self.gc_visit(f.ty, &format!("({place}).f_{}", self.sym(f.name)), gv) {
                        out.push_str(&code);
                        out.push(' ');
                    }
                }
                (!out.is_empty()).then_some(out)
            }
            Ty::Array(e) => {
                let ect = self.ctype(e);
                let body = self.gc_visit(e, "d[i]", gv)?;
                let name = format!("gv_{}", ty.0);
                if !gv.contains_key(&ty) {
                    gv.insert(ty, String::new());
                    let code = format!("static void {name}(void *buf, int64_t len) {{\n    {ect} *d = ({ect} *)(void *)((bm_arrbuf *)buf)->data;\n    for (int64_t i = 0; i < len; i++) {{ {body} }}\n}}\n");
                    gv.insert(ty, code);
                }
                Some(format!("if (bmg_gc_buf(({place}).p, ({place}).len, {name})) {name}(({place}).p, ({place}).len);"))
            }
            Ty::Map(k, v) => {
                let (kct, vct) = (self.ctype(k), self.ctype(v));
                let kv = self.gc_visit(k, &format!("(*({kct} *)kp)"), gv);
                let vv = self.gc_visit(v, &format!("(*({vct} *)vp)"), gv);
                if kv.is_none() && vv.is_none() {
                    return None;
                }
                let (kd, vd) = (self.desc(k), self.desc(v));
                let name = format!("gv_{}", ty.0);
                if !gv.contains_key(&ty) {
                    gv.insert(ty, String::new());
                    let code = format!(
                        "static void {name}(void *buf, int64_t len) {{\n    (void)len; bm_map m = {{ (bm_mapbuf *)buf }}; bm_int i = 0; void *kp, *vp;\n    while (bm_map_next(m, {kd}, {vd}, &i, &kp, &vp)) {{ {} {} }}\n}}\n",
                        kv.unwrap_or_default(),
                        vv.unwrap_or_default()
                    );
                    gv.insert(ty, code);
                }
                Some(format!("if (bmg_gc_buf(({place}).p, 0, {name})) {name}(({place}).p, 0);"))
            }
            Ty::Set(k) => {
                let kct = self.ctype(k);
                let kv = self.gc_visit(k, &format!("(*({kct} *)kp)"), gv)?;
                let kd = self.desc(k);
                let name = format!("gv_{}", ty.0);
                if !gv.contains_key(&ty) {
                    gv.insert(ty, String::new());
                    let code = format!("static void {name}(void *buf, int64_t len) {{\n    (void)len; bm_map m = {{ (bm_mapbuf *)buf }}; bm_int i = 0; void *kp, *vp;\n    while (bm_map_next(m, {kd}, &bm_type_undefined, &i, &kp, &vp)) {{ {kv} }}\n}}\n");
                    gv.insert(ty, code);
                }
                Some(format!("if (bmg_gc_buf(({place}).p, 0, {name})) {name}(({place}).p, 0);"))
            }
            Ty::Rec(..) => {
                let inner = self.c.unfold(ty);
                let bn = self.box_name(ty);
                let body = self.gc_visit(inner, &format!("(({bn} *)buf)->v"), gv)?;
                let name = format!("gv_{}", ty.0);
                if !gv.contains_key(&ty) {
                    gv.insert(ty, String::new());
                    let code = format!("static void {name}(void *buf, int64_t len) {{\n    (void)len; {body}\n}}\n");
                    gv.insert(ty, code);
                }
                Some(format!("if (bmg_gc_buf({place}, 0, {name})) {name}({place}, 0);"))
            }
            _ => None,
        }
    }

    // ------------------------------------------------------------ printing

    pub(crate) fn class_string_code(&mut self, ty: TyId, place: &str) -> String {
        let to_string = self.c.interner.lookup("toString");
        if let Some(ts) = to_string
            && let Some(ClassMemberRef::Method(mm)) = self.c.class_member(ty, ts)
            && mm.params.is_empty()
            && self.c.method_ret(&mm) == STR
        {
            let call = self.method_call_code(place, ty, ts, false, Vec::new());
            return format!("({{ bm_str s_ = {call}; bm_sb_push_str(sb, s_); bm_str_release(s_); }})");
        }
        "bm_sb_push_cstr(sb, \"[object Object]\")".into()
    }

    // ------------------------------------------------------------ emission

    /// Emits queued constructors and methods.
    pub(crate) fn emit_class_work(&mut self, w: ClassWork) {
        match w {
            ClassWork::Ctor { ty, cname } => {
                let ci = self.class_inst(ty);
                let info = self.c.classes[ci.decl as usize].clone();
                let m = info.module;
                let proto = {
                    let params: Vec<FnParam> = info.ctor.params.iter().map(|p| FnParam { ty: self.c.types.subst(p.ty, &ci.subst), ..*p }).collect();
                    self.self_proto(&cname, &ci.name, &params, VOID)
                };
                let code = match info.ctor.member {
                    Some(mi) => {
                        let fd = self.c.member_fn(ci.decl, mi);
                        let params: Vec<(u32, TyId, bool)> = fd.params.iter().zip(&info.ctor.params).map(|(p, fp)| (p.span.start, self.c.types.subst(fp.ty, &ci.subst), fp.inout)).collect();
                        // Without a base class, field initializers run first; otherwise right after `super(...)`.
                        let ctor_class = Some(ty);
                        self.function_body(m, ci.subst.clone(), &params, VOID, FnBodyKind::Block(fd.body), None, Some((fd.name_span.start, ty)), ctor_class)
                    }
                    None => {
                        // Implicit constructor: `super(...args)` (if there's a base), then field initializers.
                        let mut pre = String::new();
                        if let Some(b) = info.base {
                            let bt = self.c.types.subst(b, &ci.subst);
                            let bctor = self.ctor_instance(bt);
                            let bn = self.box_name(bt);
                            let mut all = vec![format!("({bn} *)self_")];
                            all.extend((0..info.ctor.params.len()).map(|i| format!("p{i}")));
                            let _ = writeln!(pre, "    {bctor}({});", all.join(", "));
                        }
                        let body = self.function_body(m, ci.subst.clone(), &[], VOID, FnBodyKind::Init, None, Some((u32::MAX, ty)), Some(ty));
                        format!("{pre}{body}")
                    }
                };
                let _ = writeln!(self.funcs, "static {proto} {{\n{code}}}\n");
            }
            ClassWork::Method { ty, member, cname, msubst } => {
                let ci = self.class_inst(ty);
                let m = self.c.classes[ci.decl as usize].module;
                let fd = self.c.member_fn(ci.decl, member);
                let mut subst = ci.subst.clone();
                subst.extend(msubst);
                let (params, ret) = self.own_method_sig(ci.decl, member, &subst);
                let proto = self.self_proto(&cname, &ci.name, &params, ret);
                let ps: Vec<(u32, TyId, bool)> = fd.params.iter().zip(&params).map(|(p, fp)| (p.span.start, fp.ty, fp.inout)).collect();
                let code = self.function_body(m, subst, &ps, ret, FnBodyKind::Block(fd.body), None, Some((fd.name_span.start, ty)), None);
                let _ = writeln!(self.funcs, "static {proto} {{\n{code}}}\n");
            }
            ClassWork::Static { decl, member, cname, subst } => {
                let m = self.c.classes[decl as usize].module;
                let fd = self.c.member_fn(decl, member);
                let (params, ret) = self.own_method_sig(decl, member, &subst);
                let proto = self.fn_proto(&cname, &params, ret, &FxMap::default(), false);
                let ps: Vec<(u32, TyId, bool)> = fd.params.iter().zip(&params).map(|(p, fp)| (p.span.start, fp.ty, fp.inout)).collect();
                let code = self.function_body(m, subst, &ps, ret, FnBodyKind::Block(fd.body), None, None, None);
                let _ = writeln!(self.funcs, "static {proto} {{\n{code}}}\n");
            }
        }
    }

    /// Recomputes every dispatch function's cases; returns true if that queued new work.
    pub(crate) fn refresh_dispatchers(&mut self) -> bool {
        let before = self.class_work.len() + self.class_list.len();
        let mut bodies = String::new();
        let mut i = 0;
        while i < self.dispatchers.len() {
            let (dname, recv, method, params, ret) = {
                let d = &self.dispatchers[i];
                (d.name.clone(), d.recv, d.method, d.params.clone(), d.ret)
            };
            i += 1;
            let Some((recv_decl, _)) = self.c.class_of(recv) else { continue };
            let args: Vec<String> = (0..params.len()).map(|k| format!("p{k}")).collect();
            let mut cases = String::new();
            let insts: Vec<TyId> = self.class_list.clone();
            for t in insts {
                let ci = self.class_insts[&t].clone();
                if !ci.constructed || !self.c.class_descends(ci.decl, recv_decl) || self.c.upcast_to(t, recv_decl) != Some(recv) {
                    continue;
                }
                let call = self.method_call_code(&format!("(({} *)self_)", ci.name), t, method, true, args.clone());
                let _ = writeln!(cases, "    case {}: {}{call};{}", ci.cid, if ret == VOID { "" } else { "return " }, if ret == VOID { " return;" } else { "" });
            }
            let proto = self.self_proto(&dname, &self.class_insts[&recv].name.clone(), &params, ret);
            let _ = writeln!(bodies, "static {proto} {{\n    switch (((bmg_obj *)self_)->cid) {{\n{cases}    default: bm_trap(\"internal error: method dispatch\", {});\n    }}\n}}", c_string(b"dispatch"));
        }
        self.dispatch_bodies = bodies;
        self.class_work.len() + self.class_list.len() != before
    }

    /// Per-class drop, free and inspect functions, plus the id-based tables.
    pub(crate) fn emit_class_tables(&mut self) -> String {
        let mut out = String::new();
        let insts: Vec<TyId> = self.class_list.clone();
        let mut drop_cases = String::new();
        let mut free_cases = String::new();
        let mut inspect_cases = String::new();
        let mut trav_cases = String::new();
        let mut dropf_cases = String::new();
        let mut gv_fns: FxMap<TyId, String> = FxMap::default();
        for t in &insts {
            let ci = self.class_insts[t].clone();
            let info = self.c.classes[ci.decl as usize].clone();
            let fields: Vec<CField> = info.fields.iter().map(|f| CField { ty: self.c.types.subst(f.ty, &ci.subst), ..f.clone() }).collect();
            // drop: release fields, free unless weak references remain.
            let mut rel = String::new();
            for f in &fields {
                let lv = self.field_lv_of("x", *t, f);
                if f.weak {
                    if let Some(k) = self.class_member_tag(f.ty) {
                        let (is, pl) = (self.u_is(&lv, f.ty, k), self.u_payload(&lv, f.ty, k));
                        let _ = writeln!(rel, "    if ({is}) bmg_weak_release({pl});");
                    }
                } else if self.is_rc(f.ty) {
                    let _ = writeln!(rel, "    {};", self.release_code(f.ty, &lv));
                }
            }
            let _ = writeln!(out, "static void dropf_{0}(void *p) {{\n    {0} *x = p; (void)x;\n{rel}}}", ci.name);
            if ci.cyclic {
                // The extra weak count pins the memory while fields are released (a child may drop the
                // last weak reference back to this object).
                let _ = writeln!(out, "static void drop_{0}(void *p) {{\n    bmg_obj *h = p;\n    if (h->flags & BMG_COLLECTING) return;\n    h->weak++;\n    dropf_{0}(p);\n    bmg_cyc_unlink(p);\n    if (--h->weak == 0) bmg_cyc_free(p, sizeof({0}));\n}}", ci.name);
                let _ = writeln!(free_cases, "    case {}: bmg_cyc_free(p, sizeof({})); break;", ci.cid, ci.name);
                // Traversal: the strong references this object holds to cyclic objects.
                let mut visits = String::new();
                for f in &fields {
                    if f.weak {
                        continue;
                    }
                    let lv = self.field_lv_of("x", *t, f);
                    if let Some(code) = self.gc_visit(f.ty, &lv, &mut gv_fns) {
                        let _ = writeln!(visits, "    {code}");
                    }
                }
                let _ = writeln!(out, "static void gt_{0}(void *p) {{\n    {0} *x = p; (void)x;\n{visits}}}", ci.name);
                let _ = writeln!(trav_cases, "    case {}: gt_{}(p); break;", ci.cid, ci.name);
            } else {
                if self.weak_target(ci.decl) {
                    let _ = writeln!(out, "static void drop_{0}(void *p) {{\n    bmg_obj *h = p;\n    h->weak++;\n    dropf_{0}(p);\n    if (--h->weak == 0) bmg_free_small(p, sizeof({0}));\n}}", ci.name);
                } else {
                    let _ = writeln!(out, "static void drop_{0}(void *p) {{\n    dropf_{0}(p);\n    bmg_free_small(p, sizeof({0}));\n}}", ci.name);
                }
                let _ = writeln!(free_cases, "    case {}: bmg_free_small(p, sizeof({})); break;", ci.cid, ci.name);
            }
            let _ = writeln!(drop_cases, "    case {}: drop_{}(p); break;", ci.cid, ci.name);
            let _ = writeln!(dropf_cases, "    case {}: dropf_{}(p); break;", ci.cid, ci.name);
            // inspect: `Name { field: value, ... }`.
            let cls = self.c.class_names[ci.decl as usize].clone();
            let mut b = String::new();
            if fields.is_empty() {
                let _ = writeln!(b, "    bm_inspect_object(sb, depth, {}, 0, NULL, NULL, NULL);", c_string(cls.as_bytes()));
            } else {
                let names: Vec<String> = fields.iter().map(|f| c_string(self.sym(f.name).as_bytes())).collect();
                let descs: Vec<String> = fields.iter().map(|f| self.desc(f.ty)).collect();
                let mut ptrs = Vec::new();
                for (j, f) in fields.iter().enumerate() {
                    let lv = self.field_lv_of("x", *t, f);
                    if f.weak {
                        let ct = self.ctype(f.ty);
                        let _ = writeln!(b, "    {ct} w{j} = {lv};");
                        if let (Some(k), Some(u)) = (self.class_member_tag(f.ty), self.tag_of(f.ty, UNDEFINED)) {
                            let w = format!("w{j}");
                            let (is, pl, und) = (self.u_is(&w, f.ty, k), self.u_payload(&w, f.ty, k), self.u_make(f.ty, u, None));
                            let _ = writeln!(b, "    if ({is} && ((bmg_obj *){pl})->rc <= 0) {w} = {und};");
                        }
                        ptrs.push(format!("&w{j}"));
                    } else {
                        ptrs.push(format!("&{lv}"));
                    }
                }
                let _ = writeln!(b, "    static const char *const names[] = {{ {} }};", names.join(", "));
                let _ = writeln!(b, "    const bm_type *const types[] = {{ {} }};", descs.join(", "));
                let _ = writeln!(b, "    const void *const fields[] = {{ {} }};", ptrs.join(", "));
                let _ = writeln!(b, "    bm_inspect_object(sb, depth, {}, {}, names, types, fields);", c_string(cls.as_bytes()), fields.len());
            }
            let _ = writeln!(out, "static void in_{0}(bm_sb *sb, {0} *x, int depth) {{\n{b}}}", ci.name);
            let _ = writeln!(inspect_cases, "    case {}: in_{}(sb, p, depth); break;", ci.cid, ci.name);
        }
        let _ = writeln!(out, "static void bmg_obj_drop(void *p) {{\n    switch (((bmg_obj *)p)->cid) {{\n{drop_cases}    default: break;\n    }}\n}}");
        let _ = writeln!(out, "static void bmg_obj_free(void *p) {{\n    switch (((bmg_obj *)p)->cid) {{\n{free_cases}    default: break;\n    }}\n}}");
        let _ = writeln!(out, "static void bmg_obj_inspect(bm_sb *sb, void *p, int depth) {{\n    switch (((bmg_obj *)p)->cid) {{\n{inspect_cases}    default: bm_sb_push_cstr(sb, \"[Object]\"); break;\n    }}\n}}");
        let _ = writeln!(out, "static void bmg_obj_traverse(void *p) {{\n    switch (((bmg_obj *)p)->cid) {{\n{trav_cases}    default: break;\n    }}\n}}");
        let _ = writeln!(out, "static void bmg_obj_dropfields(void *p) {{\n    switch (((bmg_obj *)p)->cid) {{\n{dropf_cases}    default: break;\n    }}\n}}");
        // Buffer visitors used by the traversals (prototypes first: they can refer to each other).
        let mut gv_out = String::new();
        let mut gvs: Vec<(TyId, String)> = gv_fns.into_iter().collect();
        gvs.sort_by_key(|(t, _)| *t);
        for (_, code) in &gvs {
            let first_line = code.lines().next().unwrap_or("");
            let _ = writeln!(gv_out, "{};", first_line.trim_end_matches(" {"));
        }
        for (_, code) in &gvs {
            gv_out.push_str(code);
        }
        out = gv_out + &out;
        let decls: Vec<u32> = self.isa_decls.iter().copied().collect();
        for d in decls {
            let mut cases = String::new();
            for t in &insts {
                let ci = self.class_insts[t].clone();
                if self.c.class_descends(ci.decl, d) {
                    let _ = write!(cases, "    case {}:", ci.cid);
                }
            }
            let body = if cases.is_empty() { "    (void)cid; return false;\n".to_string() } else { format!("    switch (cid) {{\n{cases} return true;\n    default: return false;\n    }}\n") };
            let _ = writeln!(out, "static bool bmg_isa_{d}(uint32_t cid) {{\n{body}}}");
        }
        out
    }
}
