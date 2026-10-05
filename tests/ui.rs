//! UI tests: each `tests/ui/*.tov` file (or directory) is checked and its rendered
//! diagnostics compared with the matching `.out` file. Set TOV_BLESS=1 to update them.

use std::path::{Path, PathBuf};

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/ui");
    let root = root.canonicalize().expect("tests/ui exists");
    let bless = std::env::var("TOV_BLESS").is_ok();
    let mut cases: Vec<PathBuf> = std::fs::read_dir(&root)
        .unwrap()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_dir() || p.extension().map(|e| e == "tov").unwrap_or(false))
        .collect();
    cases.sort();
    let mut failed = Vec::new();
    for case in &cases {
        let result = tov::driver::check_paths(std::slice::from_ref(case), &root).expect("check runs");
        let mut out = tov::diag::render_text(&result.diags, &result.sm);
        out.push_str(&format!("{} error(s)\n", result.diags.len()));
        for d in &result.diags {
            assert!(tov::codes::lookup(d.code).is_some(), "code {} used but not registered in codes.rs", d.code);
        }
        // Output must not depend on how many threads check it.
        for threads in [1, 3, 16] {
            let r = tov::driver::check_paths_with(std::slice::from_ref(case), &root, Some(threads)).expect("check runs");
            let again = tov::diag::render_text(&r.diags, &r.sm);
            assert_eq!(again, tov::diag::render_text(&result.diags, &result.sm), "{} differs with {threads} threads", case.display());
        }
        let expected_path = case.with_extension("out");
        let expected = std::fs::read_to_string(&expected_path).unwrap_or_default();
        if out != expected {
            if bless {
                std::fs::write(&expected_path, &out).unwrap();
            } else {
                failed.push((case.clone(), expected, out));
            }
        }
    }
    // Examples must check cleanly.
    let examples = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../examples");
    let result = tov::driver::check_paths(std::slice::from_ref(&examples), &examples).expect("check runs");
    let example_errors = tov::diag::render_text(&result.diags, &result.sm);
    for (case, expected, actual) in &failed {
        eprintln!("--- {} ---\nexpected:\n{expected}\nactual:\n{actual}", case.display());
    }
    if !example_errors.is_empty() {
        eprintln!("--- examples/ must check cleanly ---\n{example_errors}");
    }
    let ok = failed.is_empty() && example_errors.is_empty();
    println!("ui: {} cases, {} failed; examples: {} files, {} errors", cases.len(), failed.len(), result.files, result.diags.len());
    if !ok {
        std::process::exit(1);
    }
}
