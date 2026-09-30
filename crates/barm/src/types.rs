//! Interned semantic types.

use crate::intern::{Interner, Sym};
use std::sync::Arc;
use crate::hash::FxMap as HashMap;

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub struct TyId(pub u32);

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Field {
    pub name: Sym,
    pub ty: TyId,
    pub optional: bool,
}

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct FnParam {
    pub ty: TyId,
    pub inout: bool,
    pub optional: bool,
}

/// Interned list handles: `Ty` stays `Copy`, the lists live once in `Types`.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Fields(u32);
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Tys(u32);
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Params(u32);

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum Ty {
    /// Produced after an error has been reported; compatible with everything to avoid cascades.
    Error,
    Never,
    Unknown,
    Void,
    Undefined,
    Bool,
    Int,
    F64,
    F32,
    I8,
    I16,
    I32,
    U8,
    U16,
    U32,
    U64,
    Str,
    StrLit(Sym),
    Array(TyId),
    Map(TyId, TyId),
    Set(TyId),
    /// Exact record; fields sorted by name symbol.
    Record(Fields),
    /// Flattened, deduplicated, sorted, at least two members.
    Union(Tys),
    Func(Params, TyId),
    /// A generic type parameter (index into the checker's parameter table).
    Param(u32),
    /// A recursive type alias, kept nominal: (alias index, type arguments).
    Rec(u32, Tys),
    /// A structural interface: (interface index, type arguments).
    Interface(u32, Tys),
    /// A class instance (a reference): (class index, type arguments).
    Class(u32, Tys),
    /// The value of `import * as ns` (module index).
    Namespace(u32),
    /// Built-in namespaces such as `Math` and `console`.
    BuiltinNs(Sym),
    /// The result of `expect(x)` in tests.
    Expect(TyId),
}

/// Hash-consed storage for lists of `T`.
struct Lists<T> {
    items: Vec<Box<[T]>>,
    map: HashMap<Box<[T]>, u32>,
}

impl<T: Copy + Eq + std::hash::Hash> Lists<T> {
    fn new() -> Self {
        Lists { items: Vec::new(), map: HashMap::default() }
    }

    /// Returns the index relative to this table (callers add their offset).
    fn intern(&mut self, xs: &[T], offset: u32) -> u32 {
        if let Some(&i) = self.map.get(xs) {
            return i;
        }
        let i = offset + self.items.len() as u32;
        let b: Box<[T]> = xs.into();
        self.items.push(b.clone());
        self.map.insert(b, i);
        i
    }

    fn clear(&mut self) {
        self.items.clear();
        self.map.clear();
    }
}

/// Type table. A worker's table can be an *overlay* on a frozen shared base: lookups fall through
/// to the base, new types go into the overlay, and `reset` drops them. Ids below the offsets
/// belong to the base.
pub struct Types {
    base: Option<Arc<Types>>,
    /// Offsets of the overlay's first type, field list, type list and parameter list.
    off: [u32; 4],
    list: Vec<Ty>,
    map: HashMap<Ty, TyId>,
    fields: Lists<Field>,
    tys: Lists<TyId>,
    params: Lists<FnParam>,
    /// Preferred display names (from type aliases).
    names: HashMap<TyId, String>,
    /// Declaration order of record fields (records themselves store fields sorted by symbol).
    field_order: HashMap<TyId, Vec<Sym>>,
}

macro_rules! prims {
    ($($name:ident = $idx:expr => $ty:expr),* $(,)?) => {
        $(pub const $name: TyId = TyId($idx);)*
        const PRIMS: &[Ty] = &[$($ty),*];
    };
}

prims! {
    ERROR = 0 => Ty::Error,
    NEVER = 1 => Ty::Never,
    UNKNOWN = 2 => Ty::Unknown,
    VOID = 3 => Ty::Void,
    UNDEFINED = 4 => Ty::Undefined,
    BOOL = 5 => Ty::Bool,
    INT = 6 => Ty::Int,
    F64 = 7 => Ty::F64,
    F32 = 8 => Ty::F32,
    I8 = 9 => Ty::I8,
    I16 = 10 => Ty::I16,
    I32 = 11 => Ty::I32,
    U8 = 12 => Ty::U8,
    U16 = 13 => Ty::U16,
    U32 = 14 => Ty::U32,
    U64 = 15 => Ty::U64,
    STR = 16 => Ty::Str,
}

impl Default for Types {
    fn default() -> Self {
        let mut t = Types {
            base: None,
            off: [0; 4],
            list: Vec::new(),
            map: HashMap::default(),
            fields: Lists::new(),
            tys: Lists::new(),
            params: Lists::new(),
            names: HashMap::default(),
            field_order: HashMap::default(),
        };
        for p in PRIMS {
            t.intern(*p);
        }
        t
    }
}

impl Types {
    /// A new overlay on a frozen base (which must not itself be an overlay).
    pub fn overlay(base: Arc<Types>) -> Types {
        assert!(base.base.is_none(), "overlays don't nest");
        let off = [base.list.len() as u32, base.fields.items.len() as u32, base.tys.items.len() as u32, base.params.items.len() as u32];
        Types {
            base: Some(base),
            off,
            list: Vec::new(),
            map: HashMap::default(),
            fields: Lists::new(),
            tys: Lists::new(),
            params: Lists::new(),
            names: HashMap::default(),
            field_order: HashMap::default(),
        }
    }

    /// Drops everything added since the overlay was created.
    pub fn reset(&mut self) {
        self.list.clear();
        self.map.clear();
        self.fields.clear();
        self.tys.clear();
        self.params.clear();
        self.names.clear();
        self.field_order.clear();
    }

    pub fn intern(&mut self, ty: Ty) -> TyId {
        if let Some(b) = &self.base
            && let Some(&id) = b.map.get(&ty)
        {
            return id;
        }
        if let Some(&id) = self.map.get(&ty) {
            return id;
        }
        let id = TyId(self.off[0] + self.list.len() as u32);
        self.list.push(ty);
        self.map.insert(ty, id);
        id
    }

    /// Finds an already-interned type without creating it.
    pub fn lookup(&self, ty: &Ty) -> Option<TyId> {
        self.base.as_ref().and_then(|b| b.map.get(ty).copied()).or_else(|| self.map.get(ty).copied())
    }

    /// Finds an already-interned union without creating it.
    pub fn lookup_union(&self, members: &[TyId]) -> Option<TyId> {
        let l = self.base.as_ref().and_then(|b| b.tys.map.get(members).copied()).or_else(|| self.tys.map.get(members).copied())?;
        self.lookup(&Ty::Union(Tys(l)))
    }

    #[inline]
    pub fn get(&self, id: TyId) -> &Ty {
        match &self.base {
            Some(b) if id.0 < self.off[0] => &b.list[id.0 as usize],
            _ => &self.list[(id.0 - self.off[0]) as usize],
        }
    }

    #[inline]
    pub fn fields(&self, f: Fields) -> &[Field] {
        match &self.base {
            Some(b) if f.0 < self.off[1] => &b.fields.items[f.0 as usize],
            _ => &self.fields.items[(f.0 - self.off[1]) as usize],
        }
    }

    #[inline]
    pub fn tys(&self, t: Tys) -> &[TyId] {
        match &self.base {
            Some(b) if t.0 < self.off[2] => &b.tys.items[t.0 as usize],
            _ => &self.tys.items[(t.0 - self.off[2]) as usize],
        }
    }

    #[inline]
    pub fn params(&self, p: Params) -> &[FnParam] {
        match &self.base {
            Some(b) if p.0 < self.off[3] => &b.params.items[p.0 as usize],
            _ => &self.params.items[(p.0 - self.off[3]) as usize],
        }
    }

    pub fn name_of(&self, ty: TyId) -> Option<&str> {
        self.names.get(&ty).or_else(|| self.base.as_ref().and_then(|b| b.names.get(&ty))).map(|s| s.as_str())
    }

    pub fn has_name(&self, ty: TyId) -> bool {
        self.name_of(ty).is_some()
    }

    pub fn set_name(&mut self, ty: TyId, name: String) {
        self.names.insert(ty, name);
    }

    /// Fields of a record in declaration order (falls back to the stored order).
    pub fn fields_in_order(&self, ty: TyId) -> Vec<Field> {
        let Ty::Record(fs) = *self.get(ty) else { return Vec::new() };
        let fields = self.fields(fs);
        match self.field_order.get(&ty).or_else(|| self.base.as_ref().and_then(|b| b.field_order.get(&ty))) {
            Some(order) => order.iter().filter_map(|n| fields.iter().find(|f| f.name == *n).copied()).collect(),
            None => fields.to_vec(),
        }
    }

    /// Records a declaration order for a record type, unless one is already known.
    pub fn note_field_order(&mut self, ty: TyId, order: Vec<Sym>) {
        let known = self.field_order.contains_key(&ty) || self.base.as_ref().map(|b| b.field_order.contains_key(&ty)).unwrap_or(false);
        if !known {
            self.field_order.insert(ty, order);
        }
    }

    pub fn tys_list(&mut self, xs: &[TyId]) -> Tys {
        if let Some(b) = &self.base
            && let Some(&i) = b.tys.map.get(xs)
        {
            return Tys(i);
        }
        Tys(self.tys.intern(xs, self.off[2]))
    }

    fn fields_list(&mut self, xs: &[Field]) -> Fields {
        if let Some(b) = &self.base
            && let Some(&i) = b.fields.map.get(xs)
        {
            return Fields(i);
        }
        Fields(self.fields.intern(xs, self.off[1]))
    }

    fn params_list(&mut self, xs: &[FnParam]) -> Params {
        if let Some(b) = &self.base
            && let Some(&i) = b.params.map.get(xs)
        {
            return Params(i);
        }
        Params(self.params.intern(xs, self.off[3]))
    }

    pub fn array(&mut self, elem: TyId) -> TyId {
        self.intern(Ty::Array(elem))
    }

    pub fn record(&mut self, mut fields: Vec<Field>) -> TyId {
        fields.sort_by_key(|f| f.name);
        let l = self.fields_list(&fields);
        self.intern(Ty::Record(l))
    }

    pub fn func(&mut self, params: Vec<FnParam>, ret: TyId) -> TyId {
        let l = self.params_list(&params);
        self.intern(Ty::Func(l, ret))
    }

    pub fn rec(&mut self, alias: u32, args: &[TyId]) -> TyId {
        let l = self.tys_list(args);
        self.intern(Ty::Rec(alias, l))
    }

    pub fn iface(&mut self, iface: u32, args: &[TyId]) -> TyId {
        let l = self.tys_list(args);
        self.intern(Ty::Interface(iface, l))
    }

    pub fn class(&mut self, class: u32, args: &[TyId]) -> TyId {
        let l = self.tys_list(args);
        self.intern(Ty::Class(class, l))
    }

    pub fn str_lit(&mut self, s: Sym) -> TyId {
        self.intern(Ty::StrLit(s))
    }

    /// Builds a normalized union.
    pub fn union(&mut self, members: &[TyId]) -> TyId {
        if members.len() == 1 && !matches!(self.get(members[0]), Ty::Union(_) | Ty::Never) {
            return members[0];
        }
        let mut out: Vec<TyId> = Vec::with_capacity(members.len() + 2);
        for &m in members {
            match *self.get(m) {
                Ty::Error => return ERROR,
                Ty::Unknown => return UNKNOWN,
                Ty::Never => {}
                Ty::Union(ms) => out.extend_from_slice(self.tys(ms)),
                _ => out.push(m),
            }
        }
        out.sort();
        out.dedup();
        if out.contains(&STR) {
            let types = &*self;
            out.retain(|&m| !matches!(types.get(m), Ty::StrLit(_)));
        }
        match out.len() {
            0 => NEVER,
            1 => out[0],
            _ => {
                let l = self.tys_list(&out);
                self.intern(Ty::Union(l))
            }
        }
    }

    pub fn members(&self, ty: TyId) -> Vec<TyId> {
        match *self.get(ty) {
            Ty::Union(ms) => self.tys(ms).to_vec(),
            Ty::Never => Vec::new(),
            _ => vec![ty],
        }
    }

    pub fn has_undefined(&self, ty: TyId) -> bool {
        ty == UNDEFINED || matches!(*self.get(ty), Ty::Union(ms) if self.tys(ms).contains(&UNDEFINED))
    }

    pub fn without_undefined(&mut self, ty: TyId) -> TyId {
        match *self.get(ty) {
            Ty::Union(ms) => {
                if !self.tys(ms).contains(&UNDEFINED) {
                    return ty;
                }
                let rest: Vec<TyId> = self.tys(ms).iter().copied().filter(|&m| m != UNDEFINED).collect();
                self.union(&rest)
            }
            _ if ty == UNDEFINED => NEVER,
            _ => ty,
        }
    }

    pub fn optional(&mut self, ty: TyId) -> TyId {
        if self.has_undefined(ty) {
            return ty;
        }
        self.union(&[ty, UNDEFINED])
    }

    pub fn is_int(&self, ty: TyId) -> bool {
        matches!(self.get(ty), Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64)
    }

    pub fn is_float(&self, ty: TyId) -> bool {
        matches!(self.get(ty), Ty::F64 | Ty::F32)
    }

    pub fn is_numeric(&self, ty: TyId) -> bool {
        self.is_int(ty) || self.is_float(ty)
    }

    pub fn is_string(&self, ty: TyId) -> bool {
        matches!(self.get(ty), Ty::Str | Ty::StrLit(_))
    }

    /// Primitive = compared and tested by value, never "present/absent" by truthiness.
    pub fn is_primitive(&self, ty: TyId) -> bool {
        self.is_numeric(ty) || self.is_string(ty) || ty == BOOL
    }

    pub fn is_error(&self, ty: TyId) -> bool {
        ty == ERROR
    }

    /// Replaces generic parameters according to `map`.
    pub fn subst(&mut self, ty: TyId, map: &HashMap<u32, TyId>) -> TyId {
        if map.is_empty() {
            return ty;
        }
        match *self.get(ty) {
            Ty::Param(p) => map.get(&p).copied().unwrap_or(ty),
            Ty::Array(e) => {
                let e = self.subst(e, map);
                self.array(e)
            }
            Ty::Map(k, v) => {
                let k = self.subst(k, map);
                let v = self.subst(v, map);
                self.intern(Ty::Map(k, v))
            }
            Ty::Set(e) => {
                let e = self.subst(e, map);
                self.intern(Ty::Set(e))
            }
            Ty::Record(fs) => {
                let fields: Vec<Field> = self.fields(fs).to_vec();
                let fields = fields.into_iter().map(|f| Field { ty: self.subst(f.ty, map), ..f }).collect();
                self.record(fields)
            }
            Ty::Union(ms) => {
                let ms: Vec<TyId> = self.tys(ms).to_vec();
                let ms: Vec<TyId> = ms.into_iter().map(|m| self.subst(m, map)).collect();
                self.union(&ms)
            }
            Ty::Func(ps, ret) => {
                let params: Vec<FnParam> = self.params(ps).to_vec();
                let params = params.into_iter().map(|p| FnParam { ty: self.subst(p.ty, map), ..p }).collect();
                let ret = self.subst(ret, map);
                self.func(params, ret)
            }
            Ty::Rec(d, args) => {
                let args: Vec<TyId> = self.tys(args).to_vec();
                let args: Vec<TyId> = args.into_iter().map(|a| self.subst(a, map)).collect();
                self.rec(d, &args)
            }
            Ty::Interface(d, args) => {
                let args: Vec<TyId> = self.tys(args).to_vec();
                let args: Vec<TyId> = args.into_iter().map(|a| self.subst(a, map)).collect();
                self.iface(d, &args)
            }
            Ty::Class(d, args) => {
                let args: Vec<TyId> = self.tys(args).to_vec();
                let args: Vec<TyId> = args.into_iter().map(|a| self.subst(a, map)).collect();
                self.class(d, &args)
            }
            _ => ty,
        }
    }

    /// True if `ty` mentions any generic parameter in `params`.
    pub fn mentions(&self, ty: TyId, params: &[u32]) -> bool {
        if params.is_empty() {
            return false;
        }
        match *self.get(ty) {
            Ty::Param(p) => params.contains(&p),
            Ty::Array(e) | Ty::Set(e) | Ty::Expect(e) => self.mentions(e, params),
            Ty::Map(k, v) => self.mentions(k, params) || self.mentions(v, params),
            Ty::Record(fs) => self.fields(fs).iter().any(|f| self.mentions(f.ty, params)),
            Ty::Union(ms) => self.tys(ms).iter().any(|&m| self.mentions(m, params)),
            Ty::Func(ps, r) => self.params(ps).iter().any(|p| self.mentions(p.ty, params)) || self.mentions(r, params),
            Ty::Rec(_, args) | Ty::Interface(_, args) | Ty::Class(_, args) => self.tys(args).iter().any(|&a| self.mentions(a, params)),
            _ => false,
        }
    }

    /// Replaces string-literal types with `string` (for inferred `let` bindings and generic inference).
    pub fn widen(&mut self, ty: TyId) -> TyId {
        match *self.get(ty) {
            Ty::StrLit(_) => STR,
            Ty::Union(ms) if self.tys(ms).iter().all(|&m| matches!(self.get(m), Ty::StrLit(_)) || m == UNDEFINED) => {
                let ms: Vec<TyId> = self.tys(ms).iter().map(|&m| if m == UNDEFINED { m } else { STR }).collect();
                self.union(&ms)
            }
            _ => ty,
        }
    }
}

/// Renders types for diagnostics.
pub struct Display<'a> {
    pub types: &'a Types,
    pub interner: &'a Interner,
    /// Generic parameter names: shared base, then the worker's own.
    pub param_names: (&'a [Sym], &'a [Sym]),
    pub rec_names: &'a [String],
    pub iface_names: &'a [String],
    pub class_names: &'a [String],
    pub module_names: &'a [String],
}

impl Display<'_> {
    pub fn show(&self, ty: TyId) -> String {
        let mut out = String::new();
        self.write(ty, &mut out, false);
        out
    }

    fn write(&self, ty: TyId, out: &mut String, in_array: bool) {
        if let Some(name) = self.types.name_of(ty) {
            out.push_str(name);
            return;
        }
        match *self.types.get(ty) {
            Ty::Error => out.push_str("<error>"),
            Ty::Never => out.push_str("never"),
            Ty::Unknown => out.push_str("unknown"),
            Ty::Void => out.push_str("void"),
            Ty::Undefined => out.push_str("undefined"),
            Ty::Bool => out.push_str("bool"),
            Ty::Int => out.push_str("int"),
            Ty::F64 => out.push_str("f64"),
            Ty::F32 => out.push_str("f32"),
            Ty::I8 => out.push_str("i8"),
            Ty::I16 => out.push_str("i16"),
            Ty::I32 => out.push_str("i32"),
            Ty::U8 => out.push_str("u8"),
            Ty::U16 => out.push_str("u16"),
            Ty::U32 => out.push_str("u32"),
            Ty::U64 => out.push_str("u64"),
            Ty::Str => out.push_str("string"),
            Ty::StrLit(s) => {
                out.push('"');
                out.push_str(self.interner.get(s));
                out.push('"');
            }
            Ty::Array(e) => {
                self.write(e, out, true);
                out.push_str("[]");
            }
            Ty::Map(k, v) => {
                out.push_str("Map<");
                self.write(k, out, false);
                out.push_str(", ");
                self.write(v, out, false);
                out.push('>');
            }
            Ty::Set(e) => {
                out.push_str("Set<");
                self.write(e, out, false);
                out.push('>');
            }
            Ty::Record(fields) => {
                let fields = self.types.fields(fields);
                if fields.is_empty() {
                    out.push_str("{}");
                    return;
                }
                out.push_str("{ ");
                for (i, f) in fields.iter().enumerate() {
                    if i > 0 {
                        out.push_str(", ");
                    }
                    out.push_str(self.interner.get(f.name));
                    if f.optional {
                        out.push('?');
                    }
                    out.push_str(": ");
                    self.write(f.ty, out, false);
                }
                out.push_str(" }");
            }
            Ty::Union(ms) => {
                let ms = self.types.tys(ms);
                // `Shape | undefined` rather than the expanded variants.
                if ms.contains(&UNDEFINED) && ms.len() > 2 {
                    let rest: Vec<TyId> = ms.iter().copied().filter(|&m| m != UNDEFINED).collect();
                    if let Some(named) = self.types.lookup_union(&rest).filter(|&t| self.types.has_name(t)) {
                        if in_array {
                            out.push('(');
                        }
                        self.write(named, out, false);
                        out.push_str(" | undefined");
                        if in_array {
                            out.push(')');
                        }
                        return;
                    }
                }
                if in_array {
                    out.push('(');
                }
                // Show `undefined` last: `T | undefined` reads naturally.
                let mut ms = ms.to_vec();
                ms.sort_by_key(|&m| m == crate::types::UNDEFINED);
                for (i, &m) in ms.iter().enumerate() {
                    if i > 0 {
                        out.push_str(" | ");
                    }
                    self.write(m, out, true);
                }
                if in_array {
                    out.push(')');
                }
            }
            Ty::Func(params, ret) => {
                let params = self.types.params(params);
                if in_array {
                    out.push('(');
                }
                out.push('(');
                for (i, p) in params.iter().enumerate() {
                    if i > 0 {
                        out.push_str(", ");
                    }
                    if p.inout {
                        out.push_str("inout ");
                    }
                    self.write(p.ty, out, false);
                    if p.optional {
                        out.push('?');
                    }
                }
                out.push_str(") => ");
                self.write(ret, out, false);
                if in_array {
                    out.push(')');
                }
            }
            Ty::Param(p) => {
                let (base, local) = self.param_names;
                let name = if (p as usize) < base.len() { base[p as usize] } else { local[p as usize - base.len()] };
                out.push_str(self.interner.get(name));
            }
            Ty::Rec(d, args) | Ty::Interface(d, args) | Ty::Class(d, args) => {
                let names = match self.types.get(ty) {
                    Ty::Rec(..) => self.rec_names,
                    Ty::Interface(..) => self.iface_names,
                    _ => self.class_names,
                };
                out.push_str(&names[d as usize]);
                let args = self.types.tys(args);
                if !args.is_empty() {
                    out.push('<');
                    for (i, &a) in args.iter().enumerate() {
                        if i > 0 {
                            out.push_str(", ");
                        }
                        self.write(a, out, false);
                    }
                    out.push('>');
                }
            }
            Ty::Namespace(m) => {
                out.push_str("module ");
                out.push_str(&self.module_names[m as usize]);
            }
            Ty::BuiltinNs(s) => out.push_str(self.interner.get(s)),
            Ty::Expect(_) => out.push_str("Expectation"),
        }
    }
}
