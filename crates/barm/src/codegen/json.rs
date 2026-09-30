//! `JSON.stringify` / `JSON.parse`: a writer and a validating parser generated per type.
//!
//! Writers follow `JSON.stringify`: record fields in declaration order, `undefined` fields
//! omitted (and `null` in arrays), `NaN`/`Infinity` as `null`, pretty printing with an indent.
//! Maps with string keys are written as objects. Parsers check the input against the target type
//! (exact integers, string literals, discriminants, required fields) and fail with a message; extra
//! object keys are ignored, and `null` or a missing key reads as `undefined`.

use super::{c_string, Gen, Val};
use crate::types::*;
use std::fmt::Write;

impl<'c, 'a> Gen<'c, 'a> {
    /// `JSON.stringify(v, undefined, indent)`: an owned string.
    pub(crate) fn json_stringify(&mut self, v: Val, indent: Option<Val>) -> Val {
        let w = self.json_writer(v.ty);
        let ind = match indent {
            None => "BM_EMPTY_STR".to_string(),
            Some(iv) => match self.tget(iv.ty) {
                Ty::Str | Ty::StrLit(_) => {
                    let s = self.coerce(iv, STR);
                    s.code
                }
                _ => {
                    // A number of spaces (capped at 10, as in JavaScript).
                    let n = self.int_code(iv);
                    let t = self.tmp(STR, &format!("bmg_js_spaces({n})"), true);
                    t.code
                }
            },
        };
        let sbv = self.fresh("sb");
        let ct = self.ctype(v.ty);
        let pv = self.fresh("jv");
        self.line(format!("bm_sb {sbv} = {{0}}; {ct} {pv} = {};", v.code));
        self.line(format!("{w}(&{sbv}, &{pv}, {ind}, 0);"));
        self.tmp(STR, &format!("bm_str_from_sb(&{sbv})"), true)
    }

    fn json_writer(&mut self, t: TyId) -> String {
        let name = format!("js_{}", t.0);
        if !self.helpers_done.insert((t, 110)) {
            return name;
        }
        let ct = self.ctype(t);
        let _ = writeln!(self.protos, "static void {name}(bm_sb *sb, const {ct} *v, bm_str ind, int lvl);");
        let body = self.json_write_body(t);
        self.helpers_after.push(format!("static void {name}(bm_sb *sb, const {ct} *v, bm_str ind, int lvl) {{\n    (void)ind; (void)lvl;\n{body}}}\n"));
        name
    }

    /// Code writing `*v` (of type `t`).
    fn json_write_body(&mut self, t: TyId) -> String {
        match self.tget(t) {
            Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 => "    bm_sb_push_int(sb, (bm_int)*v);\n".into(),
            Ty::U64 => "    bm_json_number(sb, (double)*v);\n".into(),
            Ty::F64 | Ty::F32 => "    bm_json_number(sb, (double)*v);\n".into(),
            Ty::Bool => "    if (*v) bm_sb_add(sb, \"true\", 4); else bm_sb_add(sb, \"false\", 5);\n".into(),
            Ty::Str => "    bm_json_quote(sb, *v);\n".into(),
            Ty::StrLit(s) => {
                let lit = self.lit(self.sym(s).to_string().as_bytes());
                format!("    bm_json_quote(sb, {lit});\n")
            }
            Ty::Undefined | Ty::Void | Ty::Never | Ty::Error => "    bm_sb_push_cstr(sb, \"null\");\n".into(),
            Ty::Array(e) => {
                let w = self.json_writer(e);
                let ect = self.ctype(e);
                format!(
                    "    bm_int n = bm_arr_len(*v);\n    if (n == 0) {{ bm_sb_push_cstr(sb, \"[]\"); return; }}\n    bm_sb_add_char(sb, '[');\n    for (bm_int i = 0; i < n; i++) {{\n        if (i) bm_sb_add_char(sb, ',');\n        bmg_js_nl(sb, ind, lvl + 1);\n        {w}(sb, &(({ect} *)bm_arr_data(*v))[i], ind, lvl + 1);\n    }}\n    bmg_js_nl(sb, ind, lvl);\n    bm_sb_add_char(sb, ']');\n"
                )
            }
            Ty::Map(k, val) => {
                let (kd, vd) = (self.desc(k), self.desc(val));
                let w = self.json_writer(val);
                let (kct, vct) = (self.ctype(k), self.ctype(val));
                let skip = self.json_is_undefined(val, &format!("(*({vct} *)vp)"));
                format!(
                    "    bm_int i = 0; void *kp, *vp; bool first = true;\n    bm_sb_add_char(sb, '{{');\n    while (bm_map_next(*v, {kd}, {vd}, &i, &kp, &vp)) {{\n        if ({skip}) continue;\n        if (!first) bm_sb_add_char(sb, ',');\n        first = false;\n        bmg_js_nl(sb, ind, lvl + 1);\n        bm_json_quote(sb, *({kct} *)kp);\n        if (ind.p->len) bm_sb_add(sb, \": \", 2); else bm_sb_add_char(sb, ':');\n        {w}(sb, ({vct} *)vp, ind, lvl + 1);\n    }}\n    if (!first) bmg_js_nl(sb, ind, lvl);\n    bm_sb_add_char(sb, '}}');\n"
                )
            }
            Ty::Record(_) => {
                let fields = self.c.types.fields_in_order(t);
                let parts: Vec<(String, TyId, String)> = fields.iter().filter(|f| !matches!(self.tget(f.ty), Ty::Func(..))).map(|f| (self.sym(f.name).to_string(), f.ty, format!("v->f_{}", self.sym(f.name)))).collect();
                self.json_write_object(&parts)
            }
            Ty::Class(..) => {
                let fields = self.c.class_as_fields(t);
                let mut parts = Vec::new();
                for f in fields {
                    if matches!(self.tget(f.ty), Ty::Func(..)) {
                        continue;
                    }
                    let Some(crate::check::class::ClassMemberRef::Field(cf)) = self.c.class_member(t, f.name) else { continue };
                    let lv = self.class_field_lv_pub("(*v)", t, &cf);
                    parts.push((self.sym(f.name).to_string(), cf.ty, lv));
                }
                self.json_write_object(&parts)
            }
            Ty::Union(ms) if self.niche(t).is_some() => {
                let (pi, _) = self.niche(t).unwrap();
                let m = self.c.types.tys(ms)[pi];
                let w = self.json_writer(m);
                let mct = self.ctype(m);
                format!("    if (*v == NULL) {{ bm_sb_push_cstr(sb, \"null\"); return; }}\n    {mct} x = *v; {w}(sb, &x, ind, lvl);\n")
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let mut b = String::from("    switch (v->tag) {\n");
                for (i, &m) in ms.iter().enumerate() {
                    if self.is_unit(m) {
                        let body = self.json_write_body(m).replace("*v", "0");
                        let _ = writeln!(b, "    case {i}: {{ {} }} break;", body.trim());
                    } else {
                        let w = self.json_writer(m);
                        let _ = writeln!(b, "    case {i}: {w}(sb, &v->u.m{i}, ind, lvl); break;");
                    }
                }
                b.push_str("    default: bm_sb_push_cstr(sb, \"null\"); break;\n    }\n");
                b
            }
            Ty::Rec(..) => {
                let inner = self.c.unfold(t);
                let w = self.json_writer(inner);
                format!("    {w}(sb, &(*v)->v, ind, lvl);\n")
            }
            _ => "    bm_sb_push_cstr(sb, \"null\");\n".into(),
        }
    }

    fn json_write_object(&mut self, parts: &[(String, TyId, String)]) -> String {
        if parts.is_empty() {
            return "    bm_sb_push_cstr(sb, \"{}\");\n".into();
        }
        let mut b = String::from("    bool first = true;\n    bm_sb_add_char(sb, '{');\n");
        for (name, ty, lv) in parts {
            let w = self.json_writer(*ty);
            let key_text = format!("\"{name}\"");
            let key = format!("{}, {}", c_string(key_text.as_bytes()), key_text.len());
            let skip = self.json_is_undefined(*ty, lv);
            let _ = writeln!(
                b,
                "    if (!({skip})) {{\n        if (!first) bm_sb_add_char(sb, ',');\n        first = false;\n        bmg_js_nl(sb, ind, lvl + 1);\n        bm_sb_add(sb, {key});\n        if (ind.p->len) bm_sb_add(sb, \": \", 2); else bm_sb_add_char(sb, ':');\n        {w}(sb, &{lv}, ind, lvl + 1);\n    }}"
            );
        }
        b.push_str("    if (!first) bmg_js_nl(sb, ind, lvl);\n    bm_sb_add_char(sb, '}');\n");
        b
    }

    /// A C condition: the value at `place` (of type `t`) is `undefined` (omitted from objects).
    fn json_is_undefined(&mut self, t: TyId, place: &str) -> String {
        if t == UNDEFINED {
            return "true".into();
        }
        match self.tag_of(t, UNDEFINED) {
            Some(k) => self.u_is(place, t, k),
            None => "false".into(),
        }
    }

    // ------------------------------------------------------------ parsing

    /// `JSON.parse(text)` into `t`: an owned value; on bad input sets `bmg_err` to a `SyntaxError`.
    pub(crate) fn json_parse(&mut self, text: Val, t: TyId) -> Val {
        let p = self.json_parser(t);
        let jp = self.fresh("jp");
        let d = self.default_value(t);
        let out = self.tmp(t, &d, true);
        self.line(format!("bm_jp {jp}; bm_jp_init(&{jp}, {});", text.code));
        self.open(&format!("if (!({p}(&{jp}, &{}) && bm_jp_end(&{jp}))) {{", out.code));
        let msg = format!("bm_jp_error(&{jp})");
        let err = self.c.builtin_error_class("SyntaxError");
        if err != ERROR {
            let obj = self.error_object(err, &msg);
            self.line(format!("bmg_err = (void *)({obj});"));
        }
        self.close("}");
        out
    }

    fn json_parser(&mut self, t: TyId) -> String {
        let name = format!("jp_{}", t.0);
        if !self.helpers_done.insert((t, 111)) {
            return name;
        }
        let ct = self.ctype(t);
        let _ = writeln!(self.protos, "static bool {name}(bm_jp *p, {ct} *out);");
        let body = self.json_parse_body(t);
        self.helpers_after.push(format!("static bool {name}(bm_jp *p, {ct} *out) {{\n{body}}}\n"));
        name
    }

    /// Code parsing into `*out` (which holds a valid value; replaced on success).
    fn json_parse_body(&mut self, t: TyId) -> String {
        let int_range = |lo: &str, hi: &str| {
            format!(
                "    double d;\n    if (!bm_jp_number(p, &d)) return false;\n    if (d != (double)(int64_t)d || d < {lo} || d > {hi}) return bm_jp_fail(p, \"an integer\");\n    *out = ({}) d;\n    return true;\n",
                "__typeof__(*out)"
            )
        };
        match self.tget(t) {
            Ty::Int => int_range("-9223372036854775808.0", "9223372036854775807.0"),
            Ty::I8 => int_range("-128.0", "127.0"),
            Ty::I16 => int_range("-32768.0", "32767.0"),
            Ty::I32 => int_range("-2147483648.0", "2147483647.0"),
            Ty::U8 => int_range("0.0", "255.0"),
            Ty::U16 => int_range("0.0", "65535.0"),
            Ty::U32 => int_range("0.0", "4294967295.0"),
            Ty::U64 => int_range("0.0", "18446744073709551615.0"),
            Ty::F64 | Ty::F32 => "    double d;\n    if (!bm_jp_number(p, &d)) return false;\n    *out = (__typeof__(*out))d;\n    return true;\n".into(),
            Ty::Bool => "    char c = bm_jp_peek(p);\n    if (c == 't') { if (!bm_jp_word(p, \"true\")) return false; *out = true; return true; }\n    if (c == 'f') { if (!bm_jp_word(p, \"false\")) return false; *out = false; return true; }\n    return bm_jp_fail(p, \"a boolean\");\n".into(),
            Ty::Str => "    bm_str s;\n    if (!bm_jp_string(p, &s)) return false;\n    bm_str_release(*out);\n    *out = s;\n    return true;\n".into(),
            Ty::StrLit(s) => {
                let text = self.sym(s).to_string();
                let want = c_string(format!("\"{text}\"").as_bytes());
                format!(
                    "    bm_str s;\n    if (!bm_jp_string(p, &s)) return false;\n    bool ok = s.p->len == {} && memcmp(s.p->data, {}, {}) == 0;\n    bm_str_release(s);\n    return ok ? true : bm_jp_fail(p, {want});\n",
                    text.len(),
                    c_string(text.as_bytes()),
                    text.len()
                )
            }
            Ty::Undefined | Ty::Void => "    return bm_jp_word(p, \"null\");\n".into(),
            Ty::Array(e) => {
                let pe = self.json_parser(e);
                let (ect, d, ed) = (self.ctype(e), self.desc(e), self.default_value(e));
                let rel = self.release_code(t, "*out");
                let rel_e = self.release_code(e, "x");
                format!(
                    "    if (!bm_jp_char(p, '[')) return false;\n    bm_arr a = BM_EMPTY_ARR;\n    if (!bm_jp_try_char(p, ']')) {{\n        do {{\n            {ect} x = {ed};\n            if (!{pe}(p, &x)) {{ {rel_e}; bm_arr_release(a, {d}); return false; }}\n            BMG_PUSH({ect}, &a, {d}, x);\n        }} while (bm_jp_try_char(p, ','));\n        if (!bm_jp_char(p, ']')) {{ bm_arr_release(a, {d}); return false; }}\n    }}\n    {rel};\n    *out = a;\n    return true;\n"
                )
            }
            Ty::Map(k, v) => {
                let pv = self.json_parser(v);
                let (kd, vd, vct, dv) = (self.desc(k), self.desc(v), self.ctype(v), self.default_value(v));
                let rel_v = self.release_code(v, "x");
                let rel = self.release_code(t, "*out");
                format!(
                    "    if (!bm_jp_char(p, '{{')) return false;\n    bm_map m = BM_EMPTY_MAP;\n    if (!bm_jp_try_char(p, '}}')) {{\n        do {{\n            bm_str k;\n            if (!bm_jp_string(p, &k)) {{ bm_map_release(m, {kd}, {vd}); return false; }}\n            if (!bm_jp_char(p, ':')) {{ bm_str_release(k); bm_map_release(m, {kd}, {vd}); return false; }}\n            {vct} x = {dv};\n            if (!{pv}(p, &x)) {{ bm_str_release(k); {rel_v}; bm_map_release(m, {kd}, {vd}); return false; }}\n            bm_map_set(&m, {kd}, {vd}, &k, &x);\n        }} while (bm_jp_try_char(p, ','));\n        if (!bm_jp_char(p, '}}')) {{ bm_map_release(m, {kd}, {vd}); return false; }}\n    }}\n    {rel};\n    *out = m;\n    return true;\n"
                )
            }
            Ty::Record(fs) => {
                let fs = self.c.types.fields(fs).to_vec();
                let mut b = String::from("    if (!bm_jp_char(p, '{')) return false;\n");
                for (i, _) in fs.iter().enumerate() {
                    let _ = writeln!(b, "    bool seen{i} = false;");
                }
                b.push_str("    if (!bm_jp_try_char(p, '}')) {\n        do {\n            bm_str k;\n            if (!bm_jp_string(p, &k)) return false;\n            if (!bm_jp_char(p, ':')) { bm_str_release(k); return false; }\n            ");
                for (i, f) in fs.iter().enumerate() {
                    let n = self.sym(f.name).to_string();
                    let pf = self.json_parser(f.ty);
                    let _ = write!(
                        b,
                        "if (k.p->len == {} && memcmp(k.p->data, {}, {}) == 0) {{ bm_str_release(k); if (!{pf}(p, &out->f_{n})) return false; seen{i} = true; }}\n            else ",
                        n.len(),
                        c_string(n.as_bytes()),
                        n.len()
                    );
                }
                b.push_str("{ bm_str_release(k); if (!bm_jp_skip(p)) return false; }\n        } while (bm_jp_try_char(p, ','));\n        if (!bm_jp_char(p, '}')) return false;\n    }\n");
                for (i, f) in fs.iter().enumerate() {
                    if !f.optional && !self.c.types.has_undefined(f.ty) {
                        let n = self.sym(f.name);
                        let msg = c_string(format!("the field \"{n}\"").as_bytes());
                        let _ = writeln!(b, "    if (!seen{i}) return bm_jp_fail(p, {msg});");
                    }
                }
                b.push_str("    return true;\n");
                b
            }
            Ty::Union(ms) => {
                let ms: Vec<TyId> = self.c.types.tys(ms).to_vec();
                self.json_parse_union(t, &ms)
            }
            Ty::Rec(..) => {
                let inner = self.c.unfold(t);
                let pi = self.json_parser(inner);
                let bn = self.box_name(t);
                let (ict, di) = (self.ctype(inner), self.default_value(inner));
                let rel_i = self.release_code(inner, "x");
                let rel = self.release_code(t, "*out");
                format!(
                    "    {ict} x = {di};\n    if (!{pi}(p, &x)) {{ {rel_i}; return false; }}\n    {bn} *b = bmg_alloc_small(sizeof({bn})); b->rc = 1; b->v = x;\n    {rel};\n    *out = b;\n    return true;\n"
                )
            }
            _ => "    return bm_jp_fail(p, \"a supported value\");\n".into(),
        }
    }

    /// A union: `null` for `undefined`, discriminated records by their `kind`-like field,
    /// otherwise the member chosen by the value's first character.
    fn json_parse_union(&mut self, t: TyId, ms: &[TyId]) -> String {
        let mut b = String::from("    char c = bm_jp_peek(p);\n");
        let set = |g: &mut Self, k: usize, m: TyId, b: &mut String| {
            let pm = g.json_parser(m);
            let (mct, dm) = (g.ctype(m), g.default_value(m));
            let rel_m = g.release_code(m, "x");
            let rel = g.release_code(t, "*out");
            let make = g.u_make(t, k, Some("x"));
            let _ = write!(b, "{{ {mct} x = {dm}; if (!{pm}(p, &x)) {{ {rel_m}; return false; }} {rel}; *out = {make}; return true; }}");
        };
        // `undefined`: null.
        if let Some(k) = ms.iter().position(|&m| m == UNDEFINED) {
            let rel = self.release_code(t, "*out");
            let make = self.u_make(t, k, None);
            let _ = writeln!(b, "    if (c == 'n') {{ if (!bm_jp_word(p, \"null\")) return false; {rel}; *out = {make}; return true; }}");
        }
        // Records: by discriminant when there are several.
        let mut records: Vec<(usize, TyId)> = Vec::new();
        for (i, &m) in ms.iter().enumerate() {
            let u = self.c.unfold(m);
            if matches!(self.tget(u), Ty::Record(_)) || matches!(self.tget(m), Ty::Map(..)) {
                records.push((i, m));
            }
        }
        if !records.is_empty() {
            b.push_str("    if (c == '{') ");
            if records.len() == 1 {
                let (k, m) = records[0];
                set(self, k, m, &mut b);
                b.push('\n');
            } else {
                // Find a string-literal field every record has.
                let disc = self.discriminant(&records.iter().map(|r| r.1).collect::<Vec<_>>());
                match disc {
                    Some((field, lits)) => {
                        let fname = c_string(self.sym(field).as_bytes());
                        let _ = write!(b, "{{\n        bm_str dv;\n        if (!bm_jp_find_key(p, {fname}, &dv)) return bm_jp_fail(p, {});\n", c_string(format!("a \"{}\" field", self.sym(field)).as_bytes()));
                        for ((k, m), lit) in records.iter().zip(lits) {
                            let l = self.sym(lit).to_string();
                            let _ = write!(b, "        if (dv.p->len == {} && memcmp(dv.p->data, {}, {}) == 0) {{ bm_str_release(dv); ", l.len(), c_string(l.as_bytes()), l.len());
                            set(self, *k, *m, &mut b);
                            b.push_str(" }\n");
                        }
                        let _ = writeln!(b, "        bm_str_release(dv);\n        return bm_jp_fail(p, {});\n    }}", c_string(format!("a known \"{}\"", self.sym(field)).as_bytes()));
                    }
                    None => {
                        let (k, m) = records[0];
                        set(self, k, m, &mut b);
                        b.push('\n');
                    }
                }
            }
        }
        // By first character.
        let mut string_members = Vec::new();
        for (k, &m) in ms.iter().enumerate() {
            match self.tget(m) {
                Ty::Str | Ty::StrLit(_) => string_members.push((k, m)),
                Ty::Array(_) => {
                    b.push_str("    if (c == '[') ");
                    set(self, k, m, &mut b);
                    b.push('\n');
                }
                Ty::Bool => {
                    b.push_str("    if (c == 't' || c == 'f') ");
                    set(self, k, m, &mut b);
                    b.push('\n');
                }
                _ if self.c.types.is_numeric(m) => {
                    b.push_str("    if (c == '-' || (c >= '0' && c <= '9')) ");
                    set(self, k, m, &mut b);
                    b.push('\n');
                }
                _ => {}
            }
        }
        if !string_members.is_empty() {
            // A string: an exact literal member, else the `string` member.
            b.push_str("    if (c == '\"') {\n        size_t at = p->i;\n");
            let general = string_members.iter().find(|(_, m)| self.tget(*m) == Ty::Str).copied();
            for &(k, m) in &string_members {
                if let Ty::StrLit(s) = self.tget(m) {
                    let l = self.sym(s).to_string();
                    let rel = self.release_code(t, "*out");
                    let make = self.u_make(t, k, None);
                    let _ = writeln!(b, "        {{ bm_str s; if (!bm_jp_string(p, &s)) return false; bool hit = s.p->len == {} && memcmp(s.p->data, {}, {}) == 0; bm_str_release(s); if (hit) {{ {rel}; *out = {make}; return true; }} p->i = at; }}", l.len(), c_string(l.as_bytes()), l.len());
                }
            }
            match general {
                Some((k, m)) => {
                    b.push_str("        ");
                    set(self, k, m, &mut b);
                    b.push('\n');
                }
                None => {
                    let opts: Vec<String> = string_members.iter().filter_map(|(_, m)| if let Ty::StrLit(s) = self.tget(*m) { Some(format!("\"{}\"", self.sym(s))) } else { None }).collect();
                    let _ = writeln!(b, "        return bm_jp_fail(p, {});", c_string(format!("one of {}", opts.join(", ")).as_bytes()));
                }
            }
            b.push_str("    }\n");
        }
        b.push_str("    return bm_jp_fail(p, \"a value of the expected type\");\n");
        b
    }

    /// A field every record has with a distinct string literal (like `kind`).
    fn discriminant(&mut self, records: &[TyId]) -> Option<(crate::intern::Sym, Vec<crate::intern::Sym>)> {
        let first = self.c.unfold(records[0]);
        let Ty::Record(fs) = self.tget(first) else { return None };
        for f in self.c.types.fields(fs).to_vec() {
            let mut lits = Vec::new();
            for &r in records {
                let u = self.c.unfold(r);
                let Ty::Record(rfs) = self.tget(u) else { return None };
                match self.c.types.fields(rfs).iter().find(|x| x.name == f.name).map(|x| self.tget(x.ty)) {
                    Some(Ty::StrLit(l)) if !lits.contains(&l) => lits.push(l),
                    _ => break,
                }
            }
            if lits.len() == records.len() {
                return Some((f.name, lits));
            }
        }
        None
    }
}
