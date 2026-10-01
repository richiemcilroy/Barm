//! npm packages: each tests/npm/<name>.barm imports packages from tests/npm/node_modules (small
//! CommonJS, ES module and TypeScript packages) and runs; stdout must equal <name>.stdout.
//! macOS only for now (Barm runs JavaScript on the system's JavaScriptCore).

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    if !cfg!(target_vendor = "apple") {
        println!("npm: skipped (macOS only)");
        return;
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let dir = root.join("tests/npm");
    // the Node-API fixture: a C addon, compiled here (binaries aren't checked in)
    let addon = dir.join("node_modules/napi-addon");
    let built = addon.join("build/addon.node");
    let stale = std::fs::metadata(&built).and_then(|b| Ok(b.modified()? < std::fs::metadata(addon.join("addon.c"))?.modified()?)).unwrap_or(true);
    if stale {
        std::fs::create_dir_all(addon.join("build")).unwrap();
        let ok = Command::new("cc").args(["-O1", "-bundle", "-undefined", "dynamic_lookup", "-o"]).arg(&built).arg(addon.join("addon.c")).status().map(|s| s.success()).unwrap_or(false);
        assert!(ok, "can't compile tests/npm/node_modules/napi-addon/addon.c");
    }
    let mut cases: Vec<PathBuf> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "barm")).collect();
    cases.sort();
    let mut failed = Vec::new();
    for case in &cases {
        let name = case.file_stem().unwrap().to_string_lossy().into_owned();
        let opts = barm::build::Options { mode: barm::codegen::Mode::Run, unchecked: false, opt: "-O1".into(), emit_c: None, symbols: false };
        let built = match barm::build::build(std::slice::from_ref(case), &root, &opts) {
            Ok(b) => b,
            Err(barm::build::BuildError::Diagnostics(sm, d)) => {
                failed.push(format!("{name}: build failed\n{}", barm::diag::render_text(&d, &sm)));
                continue;
            }
            Err(barm::build::BuildError::Message(m)) => {
                failed.push(format!("{name}: {m}"));
                continue;
            }
        };
        let out = Command::new(&built.binary).output().expect("run binary");
        let actual = String::from_utf8_lossy(&out.stdout).into_owned();
        let expected = std::fs::read_to_string(case.with_extension("stdout")).unwrap_or_default();
        if actual != expected || !out.status.success() {
            failed.push(format!("{name}: output differs (exit {:?})\n--- expected\n{expected}--- actual\n{actual}{}", out.status.code(), String::from_utf8_lossy(&out.stderr)));
        }
    }
    for f in &failed {
        eprintln!("{f}\n");
    }
    println!("npm: {} programs, {} failed", cases.len(), failed.len());
    if !failed.is_empty() {
        std::process::exit(1);
    }
}
