//! The checker must never panic or hang, whatever the input. This mangles every example
//! and UI test file (truncations, deleted lines, swapped tokens) and checks each variant.

use std::path::{Path, PathBuf};

fn files(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in std::fs::read_dir(dir).unwrap().flatten() {
        let p = e.path();
        if p.is_dir() {
            files(&p, out);
        } else if p.extension().map(|x| x == "tov").unwrap_or(false) {
            out.push(p);
        }
    }
}

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> usize {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0 as usize
    }
}

fn variants(src: &str, rng: &mut Rng) -> Vec<String> {
    let mut out = Vec::new();
    // Truncations at many points.
    let cuts: Vec<usize> = src.char_indices().map(|(i, _)| i).step_by(3).collect();
    for &c in &cuts {
        out.push(src[..c].to_string());
    }
    // Deleted and duplicated lines.
    let lines: Vec<&str> = src.lines().collect();
    for i in 0..lines.len() {
        let mut v = lines.clone();
        v.remove(i);
        out.push(v.join("\n"));
        let mut d = lines.clone();
        d.insert(i, lines[i]);
        out.push(d.join("\n"));
    }
    // Random single-character deletions and insertions of syntax characters.
    let noise = ["{", "}", "(", ")", "[", "]", ":", "|", "=>", "?", "!", "&", "<", ">", ".", ",", "\"", "`", "${", "type ", "switch "];
    let idx: Vec<usize> = src.char_indices().map(|(i, _)| i).collect();
    for _ in 0..300 {
        let at = idx[rng.next() % idx.len()];
        let mut v = src.to_string();
        if rng.next().is_multiple_of(2) {
            let ch_len = v[at..].chars().next().map(|c| c.len_utf8()).unwrap_or(0);
            v.replace_range(at..at + ch_len, "");
        } else {
            v.insert_str(at, noise[rng.next() % noise.len()]);
        }
        out.push(v);
    }
    out
}

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let mut sources = Vec::new();
    files(&root.join("examples"), &mut sources);
    files(&root.join("tests/ui"), &mut sources);
    sources.sort();
    let tmp = std::env::temp_dir().join(format!("tov-robustness-{}", std::process::id()));
    std::fs::create_dir_all(&tmp).unwrap();
    let mut rng = Rng(0x9E3779B97F4A7C15);
    let mut count = 0;
    for src_path in &sources {
        let src = std::fs::read_to_string(src_path).unwrap();
        for v in variants(&src, &mut rng) {
            let file = tmp.join("case.tov");
            std::fs::write(&file, &v).unwrap();
            let r = std::panic::catch_unwind(|| tov::driver::check_paths(std::slice::from_ref(&file), &tmp));
            if r.is_err() {
                let keep = tmp.join(format!("panic-{count}.tov"));
                std::fs::write(&keep, &v).unwrap();
                eprintln!("panic on a variant of {} (saved to {})", src_path.display(), keep.display());
                std::process::exit(1);
            }
            count += 1;
        }
    }
    std::fs::remove_dir_all(&tmp).ok();
    println!("robustness: {count} mangled inputs, no panics");
}
