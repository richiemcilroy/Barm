use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;

const HELP: &str = "\
barm — the Barm toolchain

usage:
  barm check [paths...] [--json] [--time]   type-check .barm files (default: current directory)
  barm run [path] [-- args...]              build (cached) and run the program's `main`
  barm build [path] [-o out] [--emit-c f]   build a native binary
  barm test [path]                          build and run every `test(...)`
  barm explain <CODE>                       explain a diagnostic code
  barm version

build options: -O0 | -O1 | -O2 | -O3 (default) | -Os, --time,
               --unchecked (integer overflow wraps instead of trapping, like Rust release builds)
";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(|s| s.as_str()) {
        Some("check") => check(&args[1..]),
        Some("build") => build_cmd(&args[1..], BuildCmd::Build),
        Some("run") => build_cmd(&args[1..], BuildCmd::Run),
        Some("test") => build_cmd(&args[1..], BuildCmd::Test),
        Some("explain") => explain(args.get(1).map(|s| s.as_str())),
        Some("version" | "--version" | "-V") => {
            println!("barm {}", env!("CARGO_PKG_VERSION"));
            ExitCode::SUCCESS
        }
        Some("help" | "--help" | "-h") | None => {
            print!("{HELP}");
            ExitCode::SUCCESS
        }
        Some(other) => {
            eprintln!("unknown command `{other}`\n\n{HELP}");
            ExitCode::from(2)
        }
    }
}

fn check(args: &[String]) -> ExitCode {
    let json = args.iter().any(|a| a == "--json");
    let time = args.iter().any(|a| a == "--time");
    let mut paths: Vec<PathBuf> = args.iter().filter(|a| !a.starts_with("--")).map(PathBuf::from).collect();
    if paths.is_empty() {
        paths.push(PathBuf::from("."));
    }
    let base = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let start = Instant::now();
    let result = match barm::driver::check_paths(&paths, &base) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("error: {e}");
            return ExitCode::from(2);
        }
    };
    let elapsed = start.elapsed();
    if json {
        println!("{}", barm::diag::render_json(&result.diags, &result.sm, result.files));
    } else {
        print!("{}", barm::diag::render_text(&result.diags, &result.sm));
        let files = if result.files == 1 { "1 file".to_string() } else { format!("{} files", result.files) };
        if result.diags.is_empty() {
            println!("check: ok ({files})");
        } else {
            let mut with_errors: Vec<_> = result.diags.iter().map(|d| d.span.file).collect();
            with_errors.sort();
            with_errors.dedup();
            let n = result.diags.len();
            println!("check: {n} error{} in {} of {files}", if n == 1 { "" } else { "s" }, with_errors.len());
        }
    }
    if time {
        let secs = elapsed.as_secs_f64();
        eprintln!("checked {} lines in {:.2} ms ({:.0} lines/s)", result.lines, secs * 1000.0, result.lines as f64 / secs.max(1e-9));
        eprintln!("  load+parse {:.2} ms, check {:.2} ms", result.phases.0.as_secs_f64() * 1000.0, result.phases.1.as_secs_f64() * 1000.0);
    }
    // Exit without dropping the program's data structures; the OS reclaims the memory faster.
    use std::io::Write;
    let _ = std::io::stdout().flush();
    std::process::exit(if result.diags.is_empty() { 0 } else { 1 });
}

fn explain(code: Option<&str>) -> ExitCode {
    let Some(code) = code else {
        eprintln!("usage: barm explain <CODE>");
        return ExitCode::from(2);
    };
    match barm::codes::lookup(code) {
        Some(text) => {
            println!("{}: {text}", code.to_ascii_uppercase());
            ExitCode::SUCCESS
        }
        None => {
            eprintln!("unknown diagnostic code `{code}`");
            ExitCode::from(2)
        }
    }
}

#[derive(PartialEq)]
enum BuildCmd {
    Build,
    Run,
    Test,
}

fn build_cmd(args: &[String], cmd: BuildCmd) -> ExitCode {
    let (ours, program_args) = match args.iter().position(|a| a == "--") {
        Some(i) => (&args[..i], &args[i + 1..]),
        None => (args, &args[args.len()..]),
    };
    let mut paths = Vec::new();
    let mut out: Option<PathBuf> = None;
    let mut emit_c = None;
    let mut opt = "-O3".to_string();
    let mut unchecked = false;
    let mut time = false;
    let mut i = 0;
    while i < ours.len() {
        match ours[i].as_str() {
            "-o" => {
                i += 1;
                out = ours.get(i).map(PathBuf::from);
            }
            "--emit-c" => {
                i += 1;
                emit_c = ours.get(i).map(PathBuf::from);
            }
            "--time" => time = true,
            "--unchecked" => unchecked = true,
            o if o.starts_with("-O") => opt = o.to_string(),
            p => paths.push(PathBuf::from(p)),
        }
        i += 1;
    }
    if paths.is_empty() {
        paths.push(PathBuf::from("."));
    }
    let base = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let mode = if cmd == BuildCmd::Test { barm::codegen::Mode::Test } else { barm::codegen::Mode::Run };
    let opts = barm::build::Options { mode, unchecked, opt, emit_c };
    let built = match barm::build::build(&paths, &base, &opts) {
        Ok(b) => b,
        Err(barm::build::BuildError::Diagnostics(sm, diags)) => {
            print!("{}", barm::diag::render_text(&diags, &sm));
            let n = diags.len();
            println!("build: {n} error{}", if n == 1 { "" } else { "s" });
            return ExitCode::from(1);
        }
        Err(barm::build::BuildError::Message(m)) => {
            eprintln!("error: {m}");
            return ExitCode::from(2);
        }
    };
    if time {
        let (c, g, cc) = built.timings;
        eprintln!(
            "check {:.1} ms, generate {:.1} ms, C compile {:.1} ms{}",
            c.as_secs_f64() * 1000.0,
            g.as_secs_f64() * 1000.0,
            cc.as_secs_f64() * 1000.0,
            if built.cached { " (cached binary)" } else { "" }
        );
    }
    match cmd {
        BuildCmd::Build => {
            let dest = out.unwrap_or_else(|| {
                let stem = paths[0].file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "main".into());
                PathBuf::from(if stem == "." || stem.is_empty() { "main".to_string() } else { stem })
            });
            if let Err(e) = std::fs::copy(&built.binary, &dest) {
                eprintln!("error: can't write {}: {e}", dest.display());
                return ExitCode::from(2);
            }
            println!("build: {}", dest.display());
            ExitCode::SUCCESS
        }
        BuildCmd::Run | BuildCmd::Test => {
            let status = std::process::Command::new(&built.binary).args(program_args).status();
            match status {
                Ok(s) => match s.code() {
                    Some(c) => ExitCode::from(c.clamp(0, 255) as u8),
                    None => ExitCode::from(134),
                },
                Err(e) => {
                    eprintln!("error: can't run {}: {e}", built.binary.display());
                    ExitCode::from(2)
                }
            }
        }
    }
}
