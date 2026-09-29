//! Syntax tree for one module. Nodes live in flat arenas and refer to each other by index.

use crate::intern::Sym;
use crate::source::Span;

pub type ExprId = u32;
pub type StmtId = u32;
pub type TypeId = u32;

#[derive(Default)]
pub struct Ast {
    pub exprs: Vec<Expr>,
    pub stmts: Vec<Stmt>,
    pub types: Vec<TypeExpr>,
    pub items: Vec<Item>,
}

impl Ast {
    pub fn expr(&self, id: ExprId) -> &Expr {
        &self.exprs[id as usize]
    }
    pub fn stmt(&self, id: StmtId) -> &Stmt {
        &self.stmts[id as usize]
    }
    pub fn ty(&self, id: TypeId) -> &TypeExpr {
        &self.types[id as usize]
    }
}

pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Pow,
    /// `==` (rejected: use `===`)
    LooseEq,
    /// `!=` (rejected: use `!==`)
    LooseNe,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
    Nullish,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    UShr,
    In,
    Instanceof,
}

impl BinOp {
    pub fn as_str(self) -> &'static str {
        use BinOp::*;
        match self {
            Add => "+",
            Sub => "-",
            Mul => "*",
            Div => "/",
            Rem => "%",
            Pow => "**",
            LooseEq => "==",
            LooseNe => "!=",
            Eq => "===",
            Ne => "!==",
            Lt => "<",
            Gt => ">",
            Le => "<=",
            Ge => ">=",
            And => "&&",
            Or => "||",
            Nullish => "??",
            BitAnd => "&",
            BitOr => "|",
            BitXor => "^",
            Shl => "<<",
            Shr => ">>",
            UShr => ">>>",
            In => "in",
            Instanceof => "instanceof",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UnOp {
    Neg,
    Plus,
    Not,
    BitNot,
    Void,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum AssignOp {
    Assign,
    Op(BinOp),
}

pub struct Arg {
    pub expr: ExprId,
    /// Passed as `&x` (to an `inout` parameter).
    pub by_ref: Option<Span>,
}

pub struct ObjField {
    pub name: Sym,
    pub name_span: Span,
    pub value: ExprId,
}

pub enum ArrowBody {
    Expr(ExprId),
    Block(StmtId),
}

pub struct Param {
    pub name: Sym,
    pub span: Span,
    pub ty: Option<TypeId>,
    pub inout: bool,
    pub optional: bool,
}

pub struct ArrowFn {
    pub params: Vec<Param>,
    pub ret: Option<TypeId>,
    pub body: ArrowBody,
}

pub enum ExprKind {
    Int(u64),
    Float(f64),
    /// Interned at parse time, so checking never needs to mutate the interner.
    Str(Sym),
    Template(Vec<String>, Vec<ExprId>),
    Bool(bool),
    Undefined,
    Null,
    Ident(Sym),
    Unary(UnOp, ExprId),
    Binary(BinOp, ExprId, ExprId),
    Assign(AssignOp, ExprId, ExprId),
    Update { inc: bool, prefix: bool, target: ExprId },
    Call { callee: ExprId, type_args: Vec<TypeId>, args: Vec<Arg>, optional: bool },
    New { callee: ExprId, type_args: Vec<TypeId>, args: Vec<Arg> },
    Member { obj: ExprId, name: Sym, name_span: Span, optional: bool },
    Index { obj: ExprId, index: ExprId, optional: bool },
    Object(Vec<ObjField>),
    Array(Vec<ExprId>),
    Arrow(Box<ArrowFn>),
    Cond(ExprId, ExprId, ExprId),
    As(ExprId, TypeId),
    NonNull(ExprId),
    Typeof(ExprId),
    Paren(ExprId),
    Error,
}

pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}

pub struct Case {
    pub test: Option<ExprId>,
    pub body: Vec<StmtId>,
    pub span: Span,
}

pub enum StmtKind {
    Let { mutable: bool, name: Sym, name_span: Span, ty: Option<TypeId>, init: Option<ExprId> },
    Expr(ExprId),
    If(ExprId, StmtId, Option<StmtId>),
    While(ExprId, StmtId),
    DoWhile(StmtId, ExprId),
    For { init: Option<StmtId>, cond: Option<ExprId>, step: Option<ExprId>, body: StmtId },
    ForOf { mutable: bool, name: Sym, name_span: Span, iter: ExprId, body: StmtId },
    Switch(ExprId, Vec<Case>),
    Return(Option<ExprId>),
    Break,
    Continue,
    Block(Vec<StmtId>),
    Empty,
    Error,
}

pub struct TypeExpr {
    pub kind: TypeExprKind,
    pub span: Span,
}

pub struct FieldTy {
    pub name: Sym,
    pub name_span: Span,
    pub optional: bool,
    pub ty: TypeId,
}

pub enum TypeExprKind {
    Named { ns: Option<(Sym, Span)>, name: Sym, name_span: Span, args: Vec<TypeId> },
    Array(TypeId),
    Record(Vec<FieldTy>),
    Union(Vec<TypeId>),
    StrLit(Sym),
    Func(Vec<Param>, TypeId),
    Undefined,
    Null,
    Void,
    Error,
}

pub struct TypeParam {
    pub name: Sym,
    pub span: Span,
    pub bound: Option<TypeId>,
}

pub struct FnDecl {
    pub name: Sym,
    pub name_span: Span,
    pub tparams: Vec<TypeParam>,
    pub params: Vec<Param>,
    pub ret: Option<TypeId>,
    pub body: StmtId,
}

pub struct Import {
    pub names: Vec<(Sym, Span)>,
    pub namespace: Option<(Sym, Span)>,
    pub path: String,
    pub path_span: Span,
}

pub enum ItemKind {
    Import(Import),
    Function(FnDecl),
    TypeAlias { name: Sym, name_span: Span, tparams: Vec<TypeParam>, ty: TypeId },
    Interface { name: Sym, name_span: Span, tparams: Vec<TypeParam>, members: Vec<FieldTy> },
    Const { name: Sym, name_span: Span, ty: Option<TypeId>, init: ExprId },
    Test { name: String, name_span: Span, body: ExprId },
}

pub struct Item {
    pub kind: ItemKind,
    pub span: Span,
    pub exported: bool,
}

impl Ast {
    /// Rewrites every symbol (used to merge ASTs parsed with per-thread interners).
    pub fn remap_syms(&mut self, f: &impl Fn(Sym) -> Sym) {
        let params = |ps: &mut Vec<Param>| ps.iter_mut().for_each(|p| p.name = f(p.name));
        let tparams = |ps: &mut Vec<TypeParam>| ps.iter_mut().for_each(|p| p.name = f(p.name));
        let fields = |fs: &mut Vec<FieldTy>| fs.iter_mut().for_each(|x| x.name = f(x.name));
        for e in &mut self.exprs {
            match &mut e.kind {
                ExprKind::Ident(s) | ExprKind::Str(s) => *s = f(*s),
                ExprKind::Member { name, .. } => *name = f(*name),
                ExprKind::Object(fs) => fs.iter_mut().for_each(|x| x.name = f(x.name)),
                ExprKind::Arrow(a) => params(&mut a.params),
                _ => {}
            }
        }
        for s in &mut self.stmts {
            match &mut s.kind {
                StmtKind::Let { name, .. } | StmtKind::ForOf { name, .. } => *name = f(*name),
                _ => {}
            }
        }
        for t in &mut self.types {
            match &mut t.kind {
                TypeExprKind::Named { ns, name, .. } => {
                    *name = f(*name);
                    if let Some((n, _)) = ns {
                        *n = f(*n);
                    }
                }
                TypeExprKind::Record(fs) => fields(fs),
                TypeExprKind::StrLit(s) => *s = f(*s),
                TypeExprKind::Func(ps, _) => params(ps),
                _ => {}
            }
        }
        for item in &mut self.items {
            match &mut item.kind {
                ItemKind::Import(imp) => {
                    imp.names.iter_mut().for_each(|(n, _)| *n = f(*n));
                    if let Some((n, _)) = &mut imp.namespace {
                        *n = f(*n);
                    }
                }
                ItemKind::Function(fd) => {
                    fd.name = f(fd.name);
                    tparams(&mut fd.tparams);
                    params(&mut fd.params);
                }
                ItemKind::TypeAlias { name, tparams: tps, .. } => {
                    *name = f(*name);
                    tparams(tps);
                }
                ItemKind::Interface { name, tparams: tps, members, .. } => {
                    *name = f(*name);
                    tparams(tps);
                    fields(members);
                }
                ItemKind::Const { name, .. } => *name = f(*name),
                ItemKind::Test { .. } => {}
            }
        }
    }
}
