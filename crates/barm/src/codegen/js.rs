//! JavaScript values from npm packages (`Js`): imports, conversions both ways, property access
//! and calls through the bridge (runtime/js.h).

use super::{c_string, Gen, Val};
use crate::ast::{Arg, ExprId};
use crate::check::NpmName;
use crate::hash::FxMap;
use crate::intern::Sym;
use crate::source::Span;
use crate::types::*;
use std::fmt::Write;

/// Per-program state for JavaScript values.
#[derive(Default)]
pub(crate) struct JsGen {
    /// The program imports npm packages (it embeds a bundle and links the bridge).
    pub used: bool,
    /// npm import getters by (module, name).
    imports: FxMap<(u32, NpmName), String>,
    /// Property name constants by name.
    keys: FxMap<String, String>,
    /// Conversion helpers by (type, direction).
    convs: FxMap<(TyId, bool), String>,
    /// String literal constants by text.
    lits: FxMap<String, String>,
}

impl<'c, 'a> Gen<'c, 'a> {
    fn js_used(&mut self) {
        if !self.js.used {
            self.js.used = true;
            let make = self.js_error_factory();
            let _ = writeln!(
                self.helpers,
                "static void *bmg_js_make_error(JSValueRef exc) {{ bm_str n, m; bm_js_error_parts(exc, &n, &m); void *e = {make}(n, m, exc); bm_str_release(n); bm_str_release(m); return e; }}"
            );
        }
    }

    /// The C name of the prelude's `__jsError(name, message, value)`.
    fn js_error_factory(&mut self) -> String {
        let b = self.c.modules.iter().position(|m| m.builtin).expect("prelude") as u32;
        let ast = &self.c.modules[b as usize].ast;
        let item = ast
            .items
            .iter()
            .position(|it| matches!(&it.kind, crate::ast::ItemKind::Function(f) if self.c.interner.get(f.name) == "__jsError"))
            .expect("__jsError in the prelude") as u32;
        self.instance(b, item, FxMap::default())
    }

    /// A property name constant: `bm_js_key(&kN, "name")`.
    pub(crate) fn js_key(&mut self, name: &str) -> String {
        let k = match self.js.keys.get(name) {
            Some(k) => k.clone(),
            None => {
                let k = format!("bmk{}", self.js.keys.len());
                let _ = writeln!(self.lits, "static JSStringRef {k};");
                self.js.keys.insert(name.to_string(), k.clone());
                k
            }
        };
        format!("bm_js_key(&{k}, {})", c_string(name.as_bytes()))
    }

    /// The value an npm import names (read once, then kept).
    pub(crate) fn npm_value(&mut self, m: u32, name: NpmName, span: Span, ty: TyId) -> Val {
        self.js_used();
        let f = match self.js.imports.get(&(m, name)) {
            Some(f) => f.clone(),
            None => {
                let f = format!("bmg_npm{}", self.js.imports.len());
                let spec = self.c.modules[m as usize].npm.clone().unwrap_or_default();
                let loc = self.loc(span);
                let spec_c = c_string(spec.as_bytes());
                let body = match name {
                    NpmName::Ns => format!("bm_js_import_as({spec_c}, 0, {loc})"),
                    NpmName::Default => format!("bm_js_import_as({spec_c}, 1, {loc})"),
                    NpmName::Named(s) => {
                        let text = self.sym(s).to_string();
                        let key = self.js_key(&text);
                        format!("bm_js_get_or_trap(bm_js_import_as({spec_c}, 2, {loc}), {key}, {loc})")
                    }
                };
                let _ = writeln!(self.helpers, "static JSValueRef {f}(void) {{ static JSValueRef v; if (!v) v = bm_js_retain({body}); return v; }}");
                self.js.imports.insert((m, name), f.clone());
                f
            }
        };
        let v = Val::plain(format!("{f}()"), JS);
        self.coerce(v, ty)
    }

    // ------------------------------------------------------------ Barm → JavaScript

    /// A Barm value as a JavaScript value (borrowed: kept alive by the stack, not protected).
    pub(crate) fn js_of(&mut self, v: Val) -> Val {
        self.js_used();
        let from = v.ty;
        let code = match self.tget(from) {
            Ty::Js => return Val { ty: JS, ..v },
            Ty::Bool => format!("bm_js_bool({})", v.code),
            Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 => format!("bm_js_num((double)({}))", v.code),
            Ty::Str => format!("bm_js_from_str({})", v.code),
            Ty::StrLit(s) => {
                let text = self.sym(s).to_string();
                self.js_lit(&text)
            }
            Ty::Undefined | Ty::Void | Ty::Never | Ty::Error | Ty::Unknown => "bm_js_undefined()".to_string(),
            _ => {
                let f = self.js_conv(from, true);
                format!("{f}({})", v.code)
            }
        };
        let t = self.fresh("j");
        self.line(format!("JSValueRef {t} = {code};"));
        Val::plain(t, JS)
    }

    fn js_lit(&mut self, text: &str) -> String {
        let n = match self.js.lits.get(text) {
            Some(n) => n.clone(),
            None => {
                let n = format!("bmjl{}", self.js.lits.len());
                let _ = writeln!(self.lits, "static JSValueRef {n};");
                self.js.lits.insert(text.to_string(), n.clone());
                n
            }
        };
        format!("bm_js_lit(&{n}, {}, {})", c_string(text.as_bytes()), text.len())
    }

    /// The conversion helper for `t`: to JavaScript (`JSValueRef f(T v)`, `v` borrowed) or from
    /// it (`bool f(JSValueRef v, T *out)`, `*out` owned; false if the value doesn't fit).
    fn js_conv(&mut self, t: TyId, to: bool) -> String {
        if let Some(f) = self.js.convs.get(&(t, to)) {
            return f.clone();
        }
        let f = format!("bmg_{}js{}", if to { "to" } else { "from" }, self.js.convs.len());
        self.js.convs.insert((t, to), f.clone());
        let ct = self.ctype(t);
        if to {
            let _ = writeln!(self.protos, "static JSValueRef {f}({ct} v);");
        } else {
            let _ = writeln!(self.protos, "static bool {f}(JSValueRef v, {ct} *out);");
        }
        let body = if to { self.js_to_body(t) } else { self.js_from_body(t) };
        if to {
            let _ = writeln!(self.helpers, "static JSValueRef {f}({ct} v) {{\n{body}}}");
        } else {
            let _ = writeln!(self.helpers, "static bool {f}(JSValueRef v, {ct} *out) {{\n{body}}}");
        }
        f
    }

    /// An expression converting C value `code` of type `t` to a JSValueRef.
    fn js_to_code(&mut self, t: TyId, code: &str) -> String {
        match self.tget(t) {
            Ty::Js => code.to_string(),
            Ty::Bool => format!("bm_js_bool({code})"),
            Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 => format!("bm_js_num((double)({code}))"),
            Ty::Str => format!("bm_js_from_str({code})"),
            Ty::StrLit(s) => {
                let text = self.sym(s).to_string();
                self.js_lit(&text)
            }
            Ty::Undefined | Ty::Void | Ty::Never | Ty::Error | Ty::Unknown => "bm_js_undefined()".to_string(),
            _ => {
                let f = self.js_conv(t, true);
                format!("{f}({code})")
            }
        }
    }

    fn js_to_body(&mut self, t: TyId) -> String {
        let mut b = String::new();
        match self.tget(t) {
            Ty::Array(e) => {
                let ect = self.ctype(e);
                let conv = self.js_to_code(e, "xs[i]");
                let _ = writeln!(b, "    bm_int n = bm_arr_len(v); const {ect} *xs = (const {ect} *)bm_arr_data(v);");
                let _ = writeln!(b, "    JSValueRef small[16]; JSValueRef *items = n <= 16 ? small : bm_alloc((size_t)n * sizeof(JSValueRef));");
                let _ = writeln!(b, "    for (bm_int i = 0; i < n; i++) items[i] = {conv};");
                let _ = writeln!(b, "    JSValueRef r = bm_js_array((size_t)n, items);\n    if (items != small) bm_free(items);\n    return r;");
            }
            Ty::Record(fs) => {
                let _ = fs;
                let fields = self.c.types.fields_in_order(t);
                let _ = writeln!(b, "    JSValueRef o = bm_js_object();");
                for f in fields {
                    let name = self.sym(f.name).to_string();
                    let key = self.js_key(&name);
                    let conv = self.js_to_code(f.ty, &format!("v.f_{name}"));
                    if self.c.types.has_undefined(f.ty) {
                        // an absent field stays absent
                        let _ = writeln!(b, "    {{ JSValueRef x = {conv}; if (!bm_js_is_nullish(x)) bm_js_put(o, {key}, x); }}");
                    } else {
                        let _ = writeln!(b, "    bm_js_put(o, {key}, {conv});");
                    }
                }
                b.push_str("    return o;\n");
            }
            Ty::Union(_) => {
                let ms = self.members(t);
                let tag = self.u_tag("v", t);
                let _ = writeln!(b, "    switch ({tag}) {{");
                for (k, &m) in ms.iter().enumerate() {
                    let payload = self.u_payload("v", t, k);
                    let conv = if self.is_unit(m) { self.js_to_code(m, "0") } else { self.js_to_code(m, &payload) };
                    let _ = writeln!(b, "    case {k}: return {conv};");
                }
                b.push_str("    default: return bm_js_undefined();\n    }\n");
            }
            Ty::Func(ps, r, _) => {
                let ps = self.c.types.params(ps).to_vec();
                let tramp = self.js_tramp(&ps, r);
                let _ = writeln!(b, "    return bm_js_function(v, {tramp});");
            }
            _ => b.push_str("    return bm_js_undefined();\n"),
        }
        b
    }

    /// A trampoline calling a Barm closure of this signature with JavaScript arguments.
    fn js_tramp(&mut self, ps: &[FnParam], ret: TyId) -> String {
        let f = self.fresh("bmg_jstramp");
        let fp = self.fn_ptr_type_pub(ps, ret);
        let args: Vec<String> = (0..ps.len()).map(|i| format!(", n > {i} ? a[{i}] : bm_js_undefined()")).collect();
        let call = format!("(({fp})fn->fn)(fn->env{})", args.concat());
        let body = if ret == VOID || self.is_unit(ret) {
            format!("    {call};\n    return bm_js_undefined();\n")
        } else {
            let rct = self.ctype(ret);
            let conv = self.js_to_code(ret, "r");
            let rel = self.release_code(ret, "r");
            format!("    {rct} r = {call};\n    JSValueRef out = {conv};\n    {rel};\n    return out;\n")
        };
        let _ = writeln!(self.helpers, "static JSValueRef {f}(const bm_fn *fn, size_t n, const JSValueRef *a) {{\n    (void)n; (void)a;\n{body}}}");
        f
    }

    // ------------------------------------------------------------ JavaScript → Barm

    /// `x as T` for a JavaScript `x`: converted, or a trap if it doesn't fit.
    pub(crate) fn barm_of_js(&mut self, v: Val, to: TyId, span: Span) -> Val {
        self.js_used();
        if self.tget(to) == Ty::Js {
            return v;
        }
        let f = self.js_conv(to, false);
        let ct = self.ctype(to);
        let r = self.fresh("r");
        let loc = self.loc(span);
        let want = c_string(self.c.show(to).as_bytes());
        self.line(format!("{ct} {r}; if (!{f}({}, &{r})) bm_js_type_trap({}, {want}, {loc});", v.code, v.code));
        let owned = self.is_rc(to);
        if owned {
            self.b().temps.last_mut().unwrap().push((r.clone(), to));
        }
        Val { code: r, ty: to, owned }
    }

    /// A statement block converting JSValueRef `src` into `*dst` of type `t` (owned); on failure
    /// it returns false from the helper.
    fn js_from_stmt(&mut self, t: TyId, src: &str, dst: &str) -> String {
        match self.tget(t) {
            Ty::Js => format!("{dst} = bm_js_retain({src});"),
            Ty::Str => format!("if (!bm_js_as_str({src}, &{dst})) return false;"),
            Ty::StrLit(s) => {
                let text = self.sym(s).to_string();
                let lit = c_string(text.as_bytes());
                format!("{{ bm_str s; if (!bm_js_as_str({src}, &s)) return false; bool ok = s.p->len == {} && memcmp(s.p->data, {lit}, {}) == 0; bm_str_release(s); if (!ok) return false; {dst} = 0; }}", text.len(), text.len())
            }
            Ty::Bool => format!("if (!bm_js_as_bool({src}, &{dst})) return false;"),
            Ty::F64 => format!("if (!bm_js_as_num({src}, &{dst})) return false;"),
            Ty::F32 => format!("{{ double d; if (!bm_js_as_num({src}, &d)) return false; {dst} = (float)d; }}"),
            Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 => {
                let ct = self.ctype(t);
                let (lo, hi) = match self.tget(t) {
                    Ty::I8 => ("-128.0", "127.0"),
                    Ty::I16 => ("-32768.0", "32767.0"),
                    Ty::I32 => ("-2147483648.0", "2147483647.0"),
                    Ty::U8 => ("0.0", "255.0"),
                    Ty::U16 => ("0.0", "65535.0"),
                    Ty::U32 => ("0.0", "4294967295.0"),
                    Ty::U64 => ("0.0", "18446744073709549568.0"),
                    _ => ("-9223372036854775808.0", "9223372036854774784.0"),
                };
                format!("{{ double d; if (!bm_js_as_num({src}, &d) || d != (double)(int64_t)d || d < {lo} || d > {hi}) return false; {dst} = ({ct})d; }}")
            }
            Ty::Undefined | Ty::Void => format!("if (!bm_js_is_nullish({src})) return false; {dst} = 0;"),
            _ => {
                let f = self.js_conv(t, false);
                format!("if (!{f}({src}, &{dst})) return false;")
            }
        }
    }

    fn js_from_body(&mut self, t: TyId) -> String {
        let mut b = String::new();
        match self.tget(t) {
            Ty::Array(e) => {
                let ect = self.ctype(e);
                let d = self.desc(e);
                let conv = self.js_from_stmt(e, "x", "w[i]");
                let rel = self.release_code(t, "a");
                let _ = writeln!(b, "    if (!bm_js_is_array(v)) return false;");
                let _ = writeln!(b, "    uint32_t n = bm_js_length(v); bm_arr a = BM_EMPTY_ARR;");
                let _ = writeln!(b, "    {ect} *w = ({ect} *)bm_arr_reserve_tail(&a, {d}, n);");
                // a failing element: release what was converted (the length counts them)
                let conv = conv.replace("return false;", &format!("{{ {rel}; return false; }}"));
                let _ = writeln!(b, "    for (uint32_t i = 0; i < n; i++) {{ JSValueRef exc = NULL; JSValueRef x = bm_js_index(v, i, &exc); if (!x) {{ {rel}; return false; }} {conv} a.len = i + 1; }}");
                let _ = writeln!(b, "    a.len = n; *out = a; return true;");
            }
            Ty::Record(fs) => {
                let fs = self.c.types.fields(fs).to_vec();
                let ct = self.ctype(t);
                let _ = writeln!(b, "    if (bm_js_is_nullish(v) || strcmp(bm_js_typeof(v), \"object\") != 0) return false;");
                let _ = writeln!(b, "    {ct} r;");
                // fields convert in order; on a failure, the ones done are released
                for (i, f) in fs.iter().enumerate() {
                    let name = self.sym(f.name).to_string();
                    let key = self.js_key(&name);
                    let mut undo = String::new();
                    for g in &fs[..i] {
                        let gn = self.sym(g.name).to_string();
                        let rl = self.release_code(g.ty, &format!("r.f_{gn}"));
                        let _ = write!(undo, "{rl}; ");
                    }
                    let conv = self.js_from_stmt(f.ty, "x", &format!("r.f_{name}"));
                    let conv = conv.replace("return false;", &format!("{{ {undo}return false; }}"));
                    let _ = writeln!(b, "    {{ JSValueRef exc = NULL; JSValueRef x = bm_js_get(v, {key}, &exc); if (!x) {{ {undo}return false; }} {conv} }}");
                }
                b.push_str("    *out = r; return true;\n");
            }
            Ty::Union(_) => {
                let ms = self.members(t);
                // `undefined` first (a missing value), then the members in order
                let mut order: Vec<usize> = (0..ms.len()).collect();
                order.sort_by_key(|&k| ms[k] != UNDEFINED);
                for k in order {
                    let m = ms[k];
                    if self.is_unit(m) {
                        if m == UNDEFINED {
                            let u = self.u_make(t, k, None);
                            let _ = writeln!(b, "    if (bm_js_is_nullish(v)) {{ *out = {u}; return true; }}");
                        } else {
                            let f = self.js_conv(m, false);
                            let u = self.u_make(t, k, None);
                            let mct = self.ctype(m);
                            let _ = writeln!(b, "    {{ {mct} x; if ({f}(v, &x)) {{ *out = {u}; return true; }} }}");
                        }
                    } else {
                        let mct = self.ctype(m);
                        let f = self.js_conv(m, false);
                        let u = self.u_make(t, k, Some("x"));
                        let _ = writeln!(b, "    {{ {mct} x; if ({f}(v, &x)) {{ *out = {u}; return true; }} }}");
                    }
                }
                b.push_str("    return false;\n");
            }
            _ => {
                let conv = self.js_from_stmt(t, "v", "*out");
                let _ = writeln!(b, "    {conv}\n    return true;");
            }
        }
        b
    }

    // ------------------------------------------------------------ access and calls

    /// `obj.name` on a JavaScript value.
    pub(crate) fn js_member(&mut self, obj: Val, name: Sym, optional: bool, ty: TyId, span: Span) -> Val {
        let text = self.sym(name).to_string();
        let key = self.js_key(&text);
        let loc = self.loc(span);
        let t = self.fresh("j");
        if optional {
            self.line(format!("JSValueRef {t} = bm_js_is_nullish({o}) ? bm_js_undefined() : bm_js_get_or_trap({o}, {key}, {loc});", o = obj.code));
        } else {
            self.line(format!("JSValueRef {t} = bm_js_get_or_trap({}, {key}, {loc});", obj.code));
        }
        self.coerce(Val::plain(t, JS), ty)
    }

    /// `obj[index]` on a JavaScript value.
    pub(crate) fn js_index(&mut self, obj: Val, index: Val, optional: bool, ty: TyId, span: Span) -> Val {
        let loc = self.loc(span);
        let get = match self.tget(index.ty) {
            Ty::Str | Ty::StrLit(_) => {
                let iv = self.coerce(index, STR);
                format!("bm_js_key_or_trap({}, {}, {loc})", obj.code, iv.code)
            }
            Ty::Js => format!("bm_js_key_or_trap({o}, bm_js_to_str({i}), {loc})", o = obj.code, i = index.code),
            _ => format!("bm_js_at_or_trap({}, (double)({}), {loc})", obj.code, index.code),
        };
        let t = self.fresh("j");
        if optional {
            self.line(format!("JSValueRef {t} = bm_js_is_nullish({}) ? bm_js_undefined() : {get};", obj.code));
        } else {
            self.line(format!("JSValueRef {t} = {get};"));
        }
        self.coerce(Val::plain(t, JS), ty)
    }

    /// A call on a JavaScript value (`method`: `obj.method(...)`), or `new`.
    pub(crate) fn js_call(&mut self, callee_obj: Val, method: Option<Sym>, args: &[Arg], new: bool, ty: TyId) -> Val {
        let mut vals = Vec::with_capacity(args.len());
        for a in args {
            let v = self.expr(a.expr);
            let j = self.js_of(v);
            vals.push(j.code);
        }
        // (a compound literal, not an array variable: async functions keep their locals in a frame)
        let argv = if vals.is_empty() { "NULL".to_string() } else { format!("(JSValueRef[]){{{}}}", vals.join(", ")) };
        let (x, r) = (self.fresh("jx"), self.fresh("jr"));
        let call = match (new, method) {
            (true, _) => format!("bm_js_new({}, {}, {argv}, &{x})", callee_obj.code, vals.len()),
            (false, Some(m)) => {
                let text = self.sym(m).to_string();
                let key = self.js_key(&text);
                format!("bm_js_invoke({}, {key}, {}, {argv}, &{x})", callee_obj.code, vals.len())
            }
            (false, None) => format!("bm_js_call({}, NULL, {}, {argv}, &{x})", callee_obj.code, vals.len()),
        };
        self.line(format!("JSValueRef {x} = NULL; JSValueRef {r} = {call};"));
        self.line(format!("if (!{r}) {{ bmg_err = bmg_js_make_error({x}); {r} = bm_js_undefined(); }}"));
        self.coerce(Val::plain(r, JS), ty)
    }

    /// The receiver of a JavaScript method call, or the function itself.
    pub(crate) fn js_callee(&mut self, e: ExprId) -> Val {
        let v = self.expr(e);
        self.js_of(v)
    }
}
