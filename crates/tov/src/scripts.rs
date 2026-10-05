//! `package.json` scripts, run as `bun run` runs them: `tov dev` (or `tov run dev`) finds the
//! nearest package.json above the working directory and runs `predev`, `dev` and `postdev` from
//! its directory with the system shell, printing each command first. Extra arguments are
//! appended to the main script's command. Only `scripts`, `name` and `version` are read; npm
//! packages aren't installed or imported (an import of one is error N0104).

use std::io::IsTerminal;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::sync::atomic::{AtomicI32, Ordering};

pub struct Package {
    pub path: PathBuf,
    pub dir: PathBuf,
    name: Option<String>,
    version: Option<String>,
    pub scripts: Vec<(String, String)>,
}

impl Package {
    pub fn script(&self, name: &str) -> Option<&str> {
        self.scripts.iter().find(|(n, _)| n == name).map(|(_, c)| c.as_str())
    }
}

/// The nearest package.json at or above the working directory.
pub fn find() -> Result<Option<Package>, String> {
    let Ok(cwd) = std::env::current_dir() else { return Ok(None) };
    let mut dir = cwd.as_path();
    loop {
        let path = dir.join("package.json");
        if path.is_file() {
            return load(&path).map(Some);
        }
        match dir.parent() {
            Some(p) => dir = p,
            None => return Ok(None),
        }
    }
}

fn load(path: &Path) -> Result<Package, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("can't read {}: {e}", path.display()))?;
    let json = Parser { s: text.as_bytes(), i: 0 }.document().map_err(|e| format!("{}: {e}", path.display()))?;
    let Json::Obj(fields) = json else { return Err(format!("{}: expected an object", path.display())) };
    let str_field = |key: &str| {
        fields.iter().find(|(k, _)| k == key).and_then(|(_, v)| if let Json::Str(s) = v { Some(s.clone()) } else { None })
    };
    let mut scripts = Vec::new();
    if let Some((_, Json::Obj(entries))) = fields.iter().find(|(k, _)| k == "scripts") {
        for (k, v) in entries {
            if let Json::Str(cmd) = v {
                scripts.push((k.clone(), cmd.clone()));
            }
        }
    }
    Ok(Package {
        path: path.to_path_buf(),
        dir: path.parent().unwrap_or(Path::new(".")).to_path_buf(),
        name: str_field("name"),
        version: str_field("version"),
        scripts,
    })
}

/// Runs `pre<name>`, `<name>` (with `args` appended) and `post<name>`, stopping at the first
/// that fails; the exit code is that script's.
pub fn run(pkg: &Package, name: &str, args: &[String], silent: bool) -> ExitCode {
    forward_signals();
    let color = std::io::stderr().is_terminal();
    let pre = format!("pre{name}");
    let post = format!("post{name}");
    for (script, extra) in [(pre.as_str(), &[][..]), (name, args), (post.as_str(), &[][..])] {
        let Some(cmd) = pkg.script(script) else { continue };
        let mut line = cmd.to_string();
        for a in extra {
            line.push(' ');
            line.push_str(&quote(a));
        }
        if !silent {
            if color {
                eprintln!("\x1b[0m\x1b[2m\x1b[35m$\x1b[0m \x1b[2m\x1b[1m{line}\x1b[0m");
            } else {
                eprintln!("$ {line}");
            }
        }
        let status = shell(&line).current_dir(&pkg.dir).envs(env(pkg, script, &line)).spawn().and_then(|mut child| {
            RUNNING.store(child.id() as i32, Ordering::Relaxed);
            let status = child.wait();
            RUNNING.store(0, Ordering::Relaxed);
            status
        });
        let code = match status {
            Ok(s) if s.success() => continue,
            Ok(s) => match s.code() {
                Some(c) => {
                    fail(color, script, &format!("exited with code {c}"));
                    c
                }
                None => {
                    let sig = signal_of(&s);
                    // Ctrl+C: the terminal already shows it
                    if sig != 2 {
                        let (name, why) = signal_name(sig);
                        let why = if why.is_empty() { String::new() } else if color { format!(" \x1b[2m({why})\x1b[0m") } else { format!(" ({why})") };
                        fail(color, script, &format!("was terminated by signal {name}{why}"));
                    }
                    // exit the same way, so whatever started `tov` sees the signal too
                    #[cfg(unix)]
                    unsafe {
                        signal(sig, 0);
                        kill(std::process::id() as i32, sig);
                    }
                    128 + sig
                }
            },
            Err(e) => {
                fail(color, script, &format!("couldn't start: {e}"));
                1
            }
        };
        return ExitCode::from(code.clamp(0, 255) as u8);
    }
    ExitCode::SUCCESS
}

/// `error: Script not found "name"`, as Bun says it, with the scripts there are.
pub fn not_found(pkg: Option<&Package>, name: &str) -> ExitCode {
    let color = std::io::stderr().is_terminal();
    if color {
        eprintln!("\x1b[0m\x1b[31merror\x1b[0m\x1b[2m:\x1b[0m Script not found \x1b[1m\"{name}\"\x1b[0m");
    } else {
        eprintln!("error: Script not found \"{name}\"");
    }
    match pkg {
        Some(p) if !p.scripts.is_empty() => {
            let names: Vec<&str> = p.scripts.iter().map(|(n, _)| n.as_str()).collect();
            eprintln!("  scripts in {}: {}", p.path.display(), names.join(", "));
        }
        Some(p) => eprintln!("  {} has no scripts", p.path.display()),
        None => eprintln!("  no package.json in this directory or above it, and no file named \"{name}\""),
    }
    ExitCode::from(1)
}

/// The running script's process, which gets the signals `tov` does.
static RUNNING: AtomicI32 = AtomicI32::new(0);

#[cfg(unix)]
unsafe extern "C" {
    fn kill(pid: i32, sig: i32) -> i32;
    fn signal(sig: i32, handler: usize) -> usize;
}

#[cfg(unix)]
extern "C" fn forward(sig: i32) {
    let pid = RUNNING.load(Ordering::Relaxed);
    if pid > 0 {
        unsafe {
            kill(pid, sig);
        }
    }
}

/// As Bun does: SIGHUP, SIGINT and SIGTERM go on to the script (so stopping `tov dev` stops
/// the server it started), and `tov` exits once the script has.
fn forward_signals() {
    #[cfg(unix)]
    for sig in [1, 2, 15] {
        unsafe {
            signal(sig, forward as extern "C" fn(i32) as usize);
        }
    }
}

fn fail(color: bool, script: &str, what: &str) {
    if color {
        eprintln!("\x1b[0m\x1b[31merror\x1b[0m\x1b[2m:\x1b[0m script \x1b[1m\"{script}\"\x1b[0m {what}\x1b[0m");
    } else {
        eprintln!("error: script \"{script}\" {what}");
    }
}

/// A signal's name and what it means, as Bun prints them.
fn signal_name(sig: i32) -> (String, &'static str) {
    let (name, why) = match sig {
        1 => ("SIGHUP", "Hangup"),
        3 => ("SIGQUIT", "Quit"),
        6 => ("SIGABRT", "Aborted"),
        9 => ("SIGKILL", "Forced quit"),
        11 => ("SIGSEGV", "Segmentation fault"),
        15 => ("SIGTERM", "Polite quit request"),
        _ => return (format!("{sig}"), ""),
    };
    (name.to_string(), why)
}

#[cfg(unix)]
fn signal_of(s: &std::process::ExitStatus) -> i32 {
    use std::os::unix::process::ExitStatusExt;
    s.signal().unwrap_or(0)
}

#[cfg(not(unix))]
fn signal_of(_: &std::process::ExitStatus) -> i32 {
    0
}

/// bash if there is one (as Bun uses), else sh.
fn shell(line: &str) -> Command {
    if cfg!(windows) {
        let mut c = Command::new("cmd");
        c.args(["/C", line]);
        return c;
    }
    let sh = if Path::new("/bin/bash").is_file() { "/bin/bash" } else { "/bin/sh" };
    let mut c = Command::new(sh);
    c.args(["-c", line]);
    c
}

/// npm's script environment (what Bun sets too), with this `tov` first on PATH so scripts
/// that call `tov` get the same toolchain, then each `node_modules/.bin` up the tree.
fn env(pkg: &Package, script: &str, line: &str) -> Vec<(String, String)> {
    let mut path: Vec<PathBuf> = Vec::new();
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)) {
        path.push(dir);
    }
    let mut dir = Some(pkg.dir.as_path());
    while let Some(d) = dir {
        path.push(d.join("node_modules").join(".bin"));
        dir = d.parent();
    }
    if let Some(old) = std::env::var_os("PATH") {
        path.extend(std::env::split_paths(&old));
    }
    let mut vars = vec![
        ("PATH".to_string(), std::env::join_paths(path).map(|p| p.to_string_lossy().into_owned()).unwrap_or_default()),
        ("npm_command".into(), "run-script".into()),
        ("npm_lifecycle_event".into(), script.into()),
        ("npm_lifecycle_script".into(), line.into()),
        ("npm_package_json".into(), pkg.path.display().to_string()),
        ("npm_config_local_prefix".into(), pkg.dir.display().to_string()),
    ];
    if let Some(n) = &pkg.name {
        vars.push(("npm_package_name".into(), n.clone()));
    }
    if let Some(v) = &pkg.version {
        vars.push(("npm_package_version".into(), v.clone()));
    }
    vars
}

/// An argument as the shell should see it: bare if it's plain, else double-quoted.
fn quote(arg: &str) -> String {
    let plain = !arg.is_empty() && arg.bytes().all(|b| b.is_ascii_alphanumeric() || b"-_./=:@%+,".contains(&b));
    if plain {
        return arg.to_string();
    }
    let mut out = String::from("\"");
    for c in arg.chars() {
        if matches!(c, '"' | '\\' | '$' | '`') {
            out.push('\\');
        }
        out.push(c);
    }
    out.push('"');
    out
}

enum Json {
    Null,
    Bool,
    Num,
    Str(String),
    Arr,
    Obj(Vec<(String, Json)>),
}

/// Just enough JSON for package.json: every value is parsed, only strings and objects kept.
struct Parser<'a> {
    s: &'a [u8],
    i: usize,
}

impl Parser<'_> {
    fn document(mut self) -> Result<Json, String> {
        let v = self.value()?;
        self.ws();
        if self.i < self.s.len() {
            return Err(self.err("unexpected text after the value"));
        }
        Ok(v)
    }

    fn err(&self, what: &str) -> String {
        let line = self.s[..self.i.min(self.s.len())].iter().filter(|&&b| b == b'\n').count() + 1;
        format!("invalid JSON at line {line}: {what}")
    }

    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn eat(&mut self, word: &[u8]) -> bool {
        if self.s[self.i..].starts_with(word) {
            self.i += word.len();
            true
        } else {
            false
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        self.ws();
        match self.s.get(self.i) {
            Some(b'{') => {
                self.i += 1;
                let mut fields = Vec::new();
                self.ws();
                if self.eat(b"}") {
                    return Ok(Json::Obj(fields));
                }
                loop {
                    self.ws();
                    let key = self.string()?;
                    self.ws();
                    if !self.eat(b":") {
                        return Err(self.err("expected `:`"));
                    }
                    fields.push((key, self.value()?));
                    self.ws();
                    if self.eat(b"}") {
                        return Ok(Json::Obj(fields));
                    }
                    if !self.eat(b",") {
                        return Err(self.err("expected `,` or `}`"));
                    }
                }
            }
            Some(b'[') => {
                self.i += 1;
                self.ws();
                if self.eat(b"]") {
                    return Ok(Json::Arr);
                }
                loop {
                    self.value()?;
                    self.ws();
                    if self.eat(b"]") {
                        return Ok(Json::Arr);
                    }
                    if !self.eat(b",") {
                        return Err(self.err("expected `,` or `]`"));
                    }
                }
            }
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b't') if self.eat(b"true") => Ok(Json::Bool),
            Some(b'f') if self.eat(b"false") => Ok(Json::Bool),
            Some(b'n') if self.eat(b"null") => Ok(Json::Null),
            Some(b'-' | b'0'..=b'9') => {
                while self.i < self.s.len() && matches!(self.s[self.i], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9') {
                    self.i += 1;
                }
                Ok(Json::Num)
            }
            _ => Err(self.err("expected a value")),
        }
    }

    fn string(&mut self) -> Result<String, String> {
        if !self.eat(b"\"") {
            return Err(self.err("expected a string"));
        }
        let mut out: Vec<u8> = Vec::new();
        loop {
            let Some(&b) = self.s.get(self.i) else { return Err(self.err("unterminated string")) };
            self.i += 1;
            match b {
                b'"' => return String::from_utf8(out).map_err(|_| self.err("invalid UTF-8")),
                b'\\' => {
                    let Some(&e) = self.s.get(self.i) else { return Err(self.err("unterminated string")) };
                    self.i += 1;
                    let c = match e {
                        b'"' => '"',
                        b'\\' => '\\',
                        b'/' => '/',
                        b'b' => '\u{8}',
                        b'f' => '\u{c}',
                        b'n' => '\n',
                        b'r' => '\r',
                        b't' => '\t',
                        b'u' => {
                            let hi = self.hex4()?;
                            let code = if (0xD800..0xDC00).contains(&hi) && self.eat(b"\\u") {
                                let lo = self.hex4()?;
                                0x10000 + ((hi - 0xD800) << 10) + (lo.wrapping_sub(0xDC00) & 0x3FF)
                            } else {
                                hi
                            };
                            char::from_u32(code).unwrap_or('\u{FFFD}')
                        }
                        _ => return Err(self.err("invalid escape")),
                    };
                    let mut buf = [0; 4];
                    out.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
                }
                _ => out.push(b),
            }
        }
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let digits = self.s.get(self.i..self.i + 4).and_then(|h| std::str::from_utf8(h).ok()).and_then(|h| u32::from_str_radix(h, 16).ok());
        let Some(v) = digits else { return Err(self.err("invalid \\u escape")) };
        self.i += 4;
        Ok(v)
    }
}
