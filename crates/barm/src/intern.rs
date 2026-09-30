use crate::hash::FxMap as HashMap;

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub struct Sym(pub u32);

#[derive(Default)]
pub struct Interner {
    map: HashMap<Box<str>, Sym>,
    strs: Vec<Box<str>>,
}

impl Interner {
    pub fn intern(&mut self, s: &str) -> Sym {
        if let Some(&sym) = self.map.get(s) {
            return sym;
        }
        let sym = Sym(self.strs.len() as u32);
        self.strs.push(s.into());
        self.map.insert(s.into(), sym);
        sym
    }

    pub fn get(&self, sym: Sym) -> &str {
        &self.strs[sym.0 as usize]
    }

    /// The symbol for `s`, if it was ever interned.
    pub fn lookup(&self, s: &str) -> Option<Sym> {
        self.map.get(s).copied()
    }

    pub fn len(&self) -> usize {
        self.strs.len()
    }

    pub fn is_empty(&self) -> bool {
        self.strs.is_empty()
    }
}
