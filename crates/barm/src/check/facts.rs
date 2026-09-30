//! What the checker learned about each module, recorded for code generation.
//! Only collected by `check_for_build` (single-threaded, one type table for the whole program).

use crate::ast::{ExprId, StmtId};
use crate::hash::FxMap;
use crate::intern::Sym;
use crate::types::{FnParam, TyId, ERROR};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IdentFact {
    /// A local variable or parameter, identified by the source offset of its declaring name.
    Local(u32),
    Fn(u32, u32),
    Const(u32, u32),
    Ns(u32),
    /// A class name (the right side of `instanceof`).
    Class(u32),
    Builtin,
}

#[derive(Clone, Debug)]
pub enum Callee {
    /// A module function (module, item).
    Fn(u32, u32),
    /// A function-typed value: a local, constant, record field, or any other expression.
    Value,
    /// A built-in method; the receiver type (with `undefined` removed).
    Method(TyId),
    /// `Math.x`, `console.x`, or a global built-in (`ns` is `None`).
    Builtin { ns: Option<Sym>, name: Sym },
    /// `expect(subject).name(...)`.
    Matcher(TyId),
    /// `obj.name(...)` on a class instance; `recv` is the receiver's class type. `sup`: `super.name(...)`
    /// (the base implementation, called directly).
    ClassMethod { recv: TyId, name: Sym, sup: bool },
    /// `C.name(...)`: a static method (class, member).
    StaticMethod(u32, u32),
    /// `new C(...)`: the class; the instance type is the call's return type.
    New(u32),
    /// `super(...)` in a constructor: the base class type.
    SuperCtor(TyId),
}

#[derive(Clone, Debug)]
pub struct CallFact {
    pub callee: Callee,
    /// Inferred or explicit type arguments, aligned with the callee's type parameters.
    pub targs: Vec<(u32, TyId)>,
    /// Parameter types after instantiation.
    pub params: Vec<FnParam>,
    pub rest: Option<TyId>,
    pub ret: TyId,
}

#[derive(Clone, Copy, Debug)]
pub enum MemberFact {
    /// `ns.value` for `import * as ns`.
    NsFn(u32, u32),
    NsConst(u32, u32),
    /// `Math.PI` and friends.
    MathConst,
    /// `C.NAME`: a static readonly field (class, member).
    StaticField(u32, u32),
    /// `process.env.NAME` (the variable's name).
    Env(Sym),
    /// `req.params` in a `routes` handler: the route's `:name` segments as this record type,
    /// read from the request's params map.
    RouteParams(TyId),
}

#[derive(Default)]
pub struct ModuleFacts {
    pub expr_ty: Vec<TyId>,
    pub idents: FxMap<ExprId, IdentFact>,
    pub calls: FxMap<ExprId, CallFact>,
    pub members: FxMap<ExprId, MemberFact>,
    /// Types of `let`/`const` bindings and `for...of` variables (and `catch` variables, keyed by the `try`).
    pub bindings: FxMap<StmtId, TyId>,
    /// Calls and `new` expressions that can throw (checked for an error right after).
    pub throwing: crate::hash::FxSet<ExprId>,
    /// Module variables (module, item) this module changes: code generation treats them like
    /// heap values (arguments read from them are owned, since a call may change them).
    pub mutated_globals: crate::hash::FxSet<(u32, u32)>,
}

impl ModuleFacts {
    pub fn new(exprs: usize) -> ModuleFacts {
        ModuleFacts { expr_ty: vec![ERROR; exprs], ..Default::default() }
    }
}
