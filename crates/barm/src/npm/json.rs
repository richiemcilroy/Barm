//! A small JSON parser for package.json files (object key order is kept: `exports`
//! conditions are tried in order).

#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl Json {
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(entries) => entries.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }
}

pub fn parse(text: &str) -> Result<Json, String> {
    let mut p = Parser { b: text.as_bytes(), i: 0 };
    p.ws();
    let v = p.value()?;
    p.ws();
    if p.i != p.b.len() {
        return Err(format!("unexpected text at byte {}", p.i));
    }
    Ok(v)
}

struct Parser<'a> {
    b: &'a [u8],
    i: usize,
}

impl Parser<'_> {
    fn ws(&mut self) {
        while self.i < self.b.len() && matches!(self.b[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
        // a UTF-8 byte order mark
        if self.b[self.i..].starts_with(&[0xef, 0xbb, 0xbf]) {
            self.i += 3;
            self.ws();
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        match self.b.get(self.i) {
            Some(b'{') => {
                self.i += 1;
                let mut entries = Vec::new();
                self.ws();
                if self.b.get(self.i) == Some(&b'}') {
                    self.i += 1;
                    return Ok(Json::Object(entries));
                }
                loop {
                    self.ws();
                    let Json::Str(k) = self.string()? else { unreachable!() };
                    self.ws();
                    if self.b.get(self.i) != Some(&b':') {
                        return Err(format!("expected `:` at byte {}", self.i));
                    }
                    self.i += 1;
                    self.ws();
                    let v = self.value()?;
                    entries.push((k, v));
                    self.ws();
                    match self.b.get(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b'}') => {
                            self.i += 1;
                            return Ok(Json::Object(entries));
                        }
                        _ => return Err(format!("expected `,` or `}}` at byte {}", self.i)),
                    }
                }
            }
            Some(b'[') => {
                self.i += 1;
                let mut items = Vec::new();
                self.ws();
                if self.b.get(self.i) == Some(&b']') {
                    self.i += 1;
                    return Ok(Json::Array(items));
                }
                loop {
                    self.ws();
                    items.push(self.value()?);
                    self.ws();
                    match self.b.get(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b']') => {
                            self.i += 1;
                            return Ok(Json::Array(items));
                        }
                        _ => return Err(format!("expected `,` or `]` at byte {}", self.i)),
                    }
                }
            }
            Some(b'"') => self.string(),
            Some(b't') if self.b[self.i..].starts_with(b"true") => {
                self.i += 4;
                Ok(Json::Bool(true))
            }
            Some(b'f') if self.b[self.i..].starts_with(b"false") => {
                self.i += 5;
                Ok(Json::Bool(false))
            }
            Some(b'n') if self.b[self.i..].starts_with(b"null") => {
                self.i += 4;
                Ok(Json::Null)
            }
            Some(c) if *c == b'-' || c.is_ascii_digit() => {
                let start = self.i;
                self.i += 1;
                while self.i < self.b.len() && matches!(self.b[self.i], b'0'..=b'9' | b'.' | b'e' | b'E' | b'+' | b'-') {
                    self.i += 1;
                }
                let s = std::str::from_utf8(&self.b[start..self.i]).unwrap_or("0");
                Ok(Json::Num(s.parse().unwrap_or(0.0)))
            }
            _ => Err(format!("unexpected character at byte {}", self.i)),
        }
    }

    fn string(&mut self) -> Result<Json, String> {
        if self.b.get(self.i) != Some(&b'"') {
            return Err(format!("expected a string at byte {}", self.i));
        }
        self.i += 1;
        let mut out = String::new();
        loop {
            let Some(&c) = self.b.get(self.i) else { return Err("unterminated string".into()) };
            match c {
                b'"' => {
                    self.i += 1;
                    return Ok(Json::Str(out));
                }
                b'\\' => {
                    let e = *self.b.get(self.i + 1).ok_or("unterminated string")?;
                    self.i += 2;
                    match e {
                        b'n' => out.push('\n'),
                        b't' => out.push('\t'),
                        b'r' => out.push('\r'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'u' => {
                            let hex = std::str::from_utf8(self.b.get(self.i..self.i + 4).ok_or("bad \\u escape")?).map_err(|_| "bad \\u escape")?;
                            let mut cp = u32::from_str_radix(hex, 16).map_err(|_| "bad \\u escape")?;
                            self.i += 4;
                            // surrogate pair
                            if (0xd800..0xdc00).contains(&cp) && self.b[self.i..].starts_with(b"\\u") {
                                let hex2 = std::str::from_utf8(&self.b[self.i + 2..self.i + 6]).unwrap_or("0000");
                                if let Ok(lo) = u32::from_str_radix(hex2, 16) {
                                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                    self.i += 6;
                                }
                            }
                            out.push(char::from_u32(cp).unwrap_or('\u{fffd}'));
                        }
                        other => out.push(other as char),
                    }
                }
                _ => {
                    // copy a run of plain bytes (valid UTF-8 in, valid UTF-8 out)
                    let start = self.i;
                    while self.i < self.b.len() && self.b[self.i] != b'"' && self.b[self.i] != b'\\' {
                        self.i += 1;
                    }
                    out.push_str(std::str::from_utf8(&self.b[start..self.i]).map_err(|_| "invalid UTF-8")?);
                }
            }
        }
    }
}
