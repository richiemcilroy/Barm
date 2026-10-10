#!/usr/bin/env python3
"""Cross-language micro-benchmark runner for Tov.

For every benchmark in bench/micro/<name>/ and every selected language this script
builds the program (recording compile time and binary size), runs it --runs times,
records the median wall time and peak RSS (via os.wait4), checks stdout against the
C version, prints markdown tables and writes bench/results/<timestamp>.json.

Standard library only. Usage:

    python3 bench/run.py                          # everything, 5 runs each
    python3 bench/run.py --runs 3 --only fib,sort --langs c,rust,tov
    python3 bench/run.py --list
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
REPO_DIR = BENCH_DIR.parent
MICRO_DIR = BENCH_DIR / "micro"
BUILD_DIR = BENCH_DIR / ".build"
RESULTS_DIR = BENCH_DIR / "results"
TOOLS_DIR = BENCH_DIR / ".tools"

# `rust-checked` is Rust with integer overflow checks (Tov's default semantics); `tov-unchecked`
# wraps on overflow (Rust release semantics).
LANGS = ["c", "rust", "rust-checked", "node", "bun", "scriptc", "tov", "tov-unchecked"]
SOURCE = {"c": "main.c", "rust": "main.rs", "rust-checked": "main.rs", "node": "main.ts", "bun": "main.ts", "scriptc": "main.ts", "tov": "main.tov", "tov-unchecked": "main.tov"}
EXE_SUFFIX = ".exe" if os.name == "nt" else ""

# Statuses that mean "we have timing numbers".
MEASURED = {"ok", "mismatch"}


class Skip(Exception):
    """The language can't be benchmarked here (toolchain missing, build failed, ...)."""

    def __init__(self, status: str, reason: str):
        super().__init__(reason)
        self.status = status
        self.reason = reason


@dataclass
class Build:
    cmd: list[str]  # command that runs the program
    compile_s: float | None = None
    binary_bytes: int | None = None
    stripped_bytes: int | None = None
    note: str = ""


@dataclass
class Result:
    bench: str
    lang: str
    status: str = "ok"
    reason: str = ""
    compile_s: float | None = None
    binary_bytes: int | None = None
    stripped_bytes: int | None = None
    source_lines: int | None = None
    times_s: list[float] = field(default_factory=list)
    rss_bytes: list[int] = field(default_factory=list)
    median_s: float | None = None
    min_s: float | None = None
    peak_rss_bytes: int | None = None
    output_matches: bool | None = None
    stdout: str = ""
    note: str = ""


# ---------------------------------------------------------------------------
# helpers


def which(name: str) -> str | None:
    return shutil.which(name)


def tail(text: str, lines: int = 12) -> str:
    out = [l for l in text.strip().splitlines() if l.strip()]
    return "\n".join(out[-lines:])


def head(text: str, lines: int = 12) -> str:
    out = [l for l in text.strip().splitlines() if l.strip()]
    return "\n".join(out[:lines])


def tool_version(cmd: list[str]) -> str | None:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = (p.stdout or p.stderr).strip()
    return text.splitlines()[0] if text else None


def compile_cmd(cmd: list[str], out: Path, timeout: float, env: dict | None = None) -> tuple[float, int]:
    """Run a build command; returns (seconds, binary size). Raises Skip on failure."""
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    t0 = time.perf_counter()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env, cwd=REPO_DIR)
    except subprocess.TimeoutExpired:
        raise Skip("build failed", f"build timed out after {timeout:.0f}s: {' '.join(cmd)}")
    except OSError as e:
        raise Skip("build failed", f"{cmd[0]}: {e}")
    elapsed = time.perf_counter() - t0
    if p.returncode != 0 or not out.exists():
        msg = (p.stderr + "\n" + p.stdout).strip() or f"exit code {p.returncode}"
        raise Skip("build failed", head(msg, 20))
    return elapsed, out.stat().st_size


def stripped_size(binary: Path) -> int | None:
    strip = which("strip")
    if strip is None:
        return None
    with tempfile.TemporaryDirectory() as d:
        dst = Path(d) / "stripped"
        args = [strip, "-o", str(dst), str(binary)]
        if sys.platform != "darwin":
            args = [strip, "--strip-all", "-o", str(dst), str(binary)]
        try:
            p = subprocess.run(args, capture_output=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            return None
        return dst.stat().st_size if p.returncode == 0 and dst.exists() else None


# ---------------------------------------------------------------------------
# per-language build steps


def build_c(src: Path, out: Path, a) -> Build:
    cc = os.environ.get("CC") or which("cc")
    if cc is None:
        raise Skip("skipped", "no C compiler (`cc`) on PATH")
    secs, size = compile_cmd([cc, "-std=c11", "-O2", "-o", str(out), str(src), "-lm"], out, a.build_timeout)
    return Build([str(out)], secs, size, stripped_size(out))


def build_rust(src: Path, out: Path, a, checked: bool = False) -> Build:
    if src.parent.name == "src":
        return build_cargo(src.parent.parent, out, a, checked)
    rustc = which("rustc")
    if rustc is None:
        raise Skip("skipped", "no `rustc` on PATH")
    extra = ["-C", "overflow-checks=on"] if checked else []
    secs, size = compile_cmd([rustc, "--edition", "2021", "-C", "opt-level=3", *extra, "-o", str(out), str(src)], out, a.build_timeout)
    return Build([str(out)], secs, size, stripped_size(out))


def build_cargo(crate: Path, out: Path, a, checked: bool) -> Build:
    """A benchmark whose Rust needs crates (tokio, serde): bench/micro/<name>/rust/, built with
    Cargo. Its crates are built first, untimed, as Tov's runtime is: the time is the program's
    own release build, as after an edit to it."""
    cargo = which("cargo")
    if cargo is None:
        raise Skip("skipped", "no `cargo` on PATH")
    target = BUILD_DIR / ("cargo-checked" if checked else "cargo")
    env = dict(os.environ, CARGO_TARGET_DIR=str(target))
    if checked:
        env["CARGO_PROFILE_RELEASE_OVERFLOW_CHECKS"] = "true"
    cmd = [cargo, "build", "--release", "--quiet", "--manifest-path", str(crate / "Cargo.toml")]
    built = target / "release" / (crate.parent.name + EXE_SUFFIX)
    compile_cmd(cmd, built, a.build_timeout, env)
    os.utime(crate / "src" / "main.rs")
    secs, size = compile_cmd(cmd, built, a.build_timeout, env)
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, out)
    return Build([str(out)], secs, size, stripped_size(out))


def build_node(src: Path, out: Path, a) -> Build:
    node = which("node")
    if node is None:
        raise Skip("skipped", "no `node` on PATH")
    major = int((tool_version([node, "--version"]) or "v0").lstrip("v").split(".")[0] or 0)
    if major < 23:
        raise Skip("skipped", f"node {major} can't run .ts files directly (needs >= 23.6 type stripping)")
    return Build([node, str(src)])


def build_bun(src: Path, out: Path, a) -> Build:
    bun = which("bun")
    if bun is None:
        raise Skip("skipped", "no `bun` on PATH")
    return Build([bun, str(src)])


def scriptc_bin() -> Path | None:
    local = TOOLS_DIR / "node_modules" / ".bin" / "scriptc"
    if local.exists():
        return local
    found = which("scriptc")
    return Path(found) if found else None


def build_scriptc(src: Path, out: Path, a) -> Build:
    sc = scriptc_bin()
    if sc is None:
        raise Skip("skipped", "scriptc not installed (run `npm install --prefix bench/.tools scriptc`)")
    if which("node") is None:
        raise Skip("skipped", "scriptc needs `node` on PATH")
    env = dict(os.environ)
    if not a.scriptc_cache:
        env["SCRIPTC_NO_CACHE"] = "1"  # measure a clean compile, not a cache hit
    try:
        secs, size = compile_cmd([str(sc), "build", str(src), "-o", str(out), "--no-keep-c"], out, a.build_timeout, env)
    except Skip as e:
        # A TypeScript program that scriptc refuses is a feature gap, not a harness problem.
        raise Skip("unsupported", e.reason)
    return Build([str(out)], secs, size, stripped_size(out))


def tov_bin() -> Path | None:
    for p in (REPO_DIR / "target" / "release" / ("tov" + EXE_SUFFIX), REPO_DIR / "target" / "debug" / ("tov" + EXE_SUFFIX)):
        if p.exists():
            return p
    return None


TOV_WARMED: set[tuple[str, ...]] = set()


def build_tov(src: Path, out: Path, a, unchecked: bool = False) -> Build:
    tov = tov_bin()
    if tov is None:
        raise Skip("skipped", "no tov compiler at target/release/tov (run `cargo build --release`)")
    # A cache private to this benchmark session, so programs are compiled rather than cache hits.
    # The runtime object is warmed first: like Rust's precompiled std, it's built once per
    # machine and compiler, not per program.
    env = dict(os.environ)
    env["TOV_CACHE_DIR"] = str(BUILD_DIR / ".tov-cache" / str(os.getpid()))
    extra = ["--unchecked"] if unchecked else []
    if tuple(extra) not in TOV_WARMED:
        warm = BUILD_DIR / "warm.tov"
        warm.parent.mkdir(parents=True, exist_ok=True)
        warm.write_text("function main() {}\n")
        compile_cmd([str(tov), "build", str(warm), *extra, "-o", str(BUILD_DIR / "warm")], BUILD_DIR / "warm", a.build_timeout, env)
        TOV_WARMED.add(tuple(extra))
    secs, size = compile_cmd([str(tov), "build", str(src), *extra, "-o", str(out)], out, a.build_timeout, env)
    return Build([str(out)], secs, size, stripped_size(out))


BUILDERS = {
    "c": build_c,
    "rust": build_rust,
    "rust-checked": lambda src, out, a: build_rust(src, out, a, checked=True),
    "node": build_node,
    "bun": build_bun,
    "scriptc": build_scriptc,
    "tov": build_tov,
    "tov-unchecked": lambda src, out, a: build_tov(src, out, a, unchecked=True),
}


def toolchain_versions() -> dict[str, str | None]:
    sc = scriptc_bin()
    tv = tov_bin()
    return {
        "c": tool_version([os.environ.get("CC") or "cc", "--version"]),
        "rust": tool_version(["rustc", "--version"]),
        "node": tool_version(["node", "--version"]),
        "bun": tool_version(["bun", "--version"]),
        "scriptc": tool_version([str(sc), "--version"]) if sc else None,
        "tov": tool_version([str(tv), "version"]) if tv else None,
        "rust-checked": None,
        "tov-unchecked": None,
    }


# ---------------------------------------------------------------------------
# running


def run_once(cmd: list[str], timeout: float) -> tuple[float, int, int, str, str, bool]:
    """Run cmd once. Returns (wall seconds, peak RSS bytes, exit code, stdout, stderr, timed_out)."""
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        t0 = time.perf_counter()
        proc = subprocess.Popen(cmd, stdout=out, stderr=err, stdin=subprocess.DEVNULL, cwd=REPO_DIR)
        timed_out = threading.Event()
        lock = threading.Lock()
        done = False

        def kill():
            with lock:
                if not done:
                    timed_out.set()
                    os.kill(proc.pid, signal.SIGKILL)

        timer = threading.Timer(timeout, kill)
        timer.start()
        if hasattr(os, "waitid"):
            # Wait for exit without reaping, so the pid can't be reused while the timer may still fire.
            os.waitid(os.P_PID, proc.pid, os.WEXITED | os.WNOWAIT)
        elapsed = time.perf_counter() - t0
        with lock:
            done = True
            timer.cancel()
        _, status, usage = os.wait4(proc.pid, 0)
        elapsed = min(elapsed, time.perf_counter() - t0)
        proc.returncode = os.waitstatus_to_exitcode(status)  # already reaped; keep Popen from waiting
        out.seek(0)
        err.seek(0)
        stdout = out.read().decode("utf-8", "replace")
        stderr = err.read().decode("utf-8", "replace")
    # ru_maxrss is bytes on macOS, kilobytes on Linux.
    rss = usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024
    return elapsed, rss, proc.returncode, stdout, stderr, timed_out.is_set()


def source(bench: str, lang: str) -> Path:
    src = MICRO_DIR / bench / SOURCE[lang]
    # (Rust that needs crates is a Cargo project: rust/src/main.rs)
    if lang.startswith("rust") and not src.exists():
        return MICRO_DIR / bench / "rust" / "src" / "main.rs"
    return src


def bench_one(bench: str, lang: str, a, expected: str | None) -> Result:
    src = source(bench, lang)
    r = Result(bench, lang)
    if not src.exists():
        r.status, r.reason = "skipped", f"no {SOURCE[lang]} for this benchmark"
        return r
    r.source_lines = sum(1 for l in src.read_text().splitlines() if l.strip() and not l.strip().startswith("//"))
    out = BUILD_DIR / bench / (lang + EXE_SUFFIX)
    try:
        b = BUILDERS[lang](src, out, a)
    except Skip as e:
        r.status, r.reason = e.status, e.reason
        return r
    r.compile_s, r.binary_bytes, r.stripped_bytes, r.note = b.compile_s, b.binary_bytes, b.stripped_bytes, b.note

    for i in range(a.warmup + a.runs):
        secs, rss, code, stdout, stderr, timed_out = run_once(b.cmd, a.timeout)
        if timed_out:
            r.status, r.reason = "timeout", f"run exceeded {a.timeout:.0f}s"
            break
        if code != 0:
            r.status, r.reason = "crashed", f"exit code {code}: {tail(stderr, 8)}"
            r.stdout = stdout
            break
        if i == 0:
            r.stdout = stdout
        elif stdout != r.stdout:
            r.status, r.reason = "mismatch", "output differs between runs"
        if i >= a.warmup:
            r.times_s.append(secs)
            r.rss_bytes.append(rss)

    if r.times_s and r.status in MEASURED:
        r.median_s = statistics.median(r.times_s)
        r.min_s = min(r.times_s)
        r.peak_rss_bytes = max(r.rss_bytes)
    if expected is not None and r.stdout:
        r.output_matches = r.stdout == expected
        if not r.output_matches and r.status == "ok":
            r.status = "mismatch"
            r.reason = "stdout differs from the C version"
    return r


# ---------------------------------------------------------------------------
# reporting


def fmt_ms(s: float | None) -> str:
    return "" if s is None else f"{s * 1000:.0f}"


def fmt_mb(b: int | None) -> str:
    return "" if b is None else f"{b / (1024 * 1024):.1f}"


def fmt_kb(b: int | None) -> str:
    return "" if b is None else f"{b / 1024:.0f}"


def fmt_s(s: float | None) -> str:
    return "" if s is None else f"{s:.2f}"


def summary_table(results: list[Result], benches: list[str], langs: list[str]) -> str:
    by = {(r.bench, r.lang): r for r in results}
    lines = ["| benchmark | " + " | ".join(langs) + " |", "|---|" + "---:|" * len(langs)]
    for bench in benches:
        base = by.get((bench, "c"))
        base_t = base.median_s if base and base.status == "ok" else None
        cells = []
        for lang in langs:
            r = by.get((bench, lang))
            if r is None:
                cells.append("")
            elif r.median_s is not None and r.status == "ok":
                ratio = f" ({r.median_s / base_t:.1f}×)" if base_t and lang != "c" else ""
                cells.append(f"{fmt_ms(r.median_s)} ms{ratio}")
            elif r.median_s is not None:
                cells.append(f"{fmt_ms(r.median_s)} ms ({r.status})")
            else:
                cells.append(r.status)
        lines.append(f"| {bench} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def detail_table(results: list[Result]) -> str:
    lines = [
        "| benchmark | lang | status | median ms | min ms | peak RSS MB | compile s | binary KB | stripped KB | output = C |",
        "|---|---|---|---:|---:|---:|---:|---:|---:|:---:|",
    ]
    for r in results:
        match = "" if r.output_matches is None else ("yes" if r.output_matches else "**no**")
        lines.append(
            f"| {r.bench} | {r.lang} | {r.status} | {fmt_ms(r.median_s)} | {fmt_ms(r.min_s)} | {fmt_mb(r.peak_rss_bytes)} | "
            f"{fmt_s(r.compile_s)} | {fmt_kb(r.binary_bytes)} | {fmt_kb(r.stripped_bytes)} | {match} |"
        )
    return "\n".join(lines)


def problems(results: list[Result]) -> str:
    out = []
    for r in results:
        if r.status != "ok" and r.reason:
            first = r.reason.splitlines()
            summary = first[0] if first else ""
            more = f" (+{len(first) - 1} more lines in JSON)" if len(first) > 1 else ""
            out.append(f"- {r.bench}/{r.lang}: **{r.status}** — {summary}{more}")
    return "\n".join(out)


# ---------------------------------------------------------------------------


def list_benches() -> list[str]:
    return sorted(p.name for p in MICRO_DIR.iterdir() if p.is_dir() and not p.name.startswith("."))


def parse_list(value: str | None, allowed: list[str], what: str) -> list[str]:
    if not value:
        return allowed
    items = [v.strip() for v in value.split(",") if v.strip()]
    unknown = [v for v in items if v not in allowed]
    if unknown:
        sys.exit(f"unknown {what}: {', '.join(unknown)} (available: {', '.join(allowed)})")
    return [v for v in allowed if v in items]


def main() -> None:
    all_benches = list_benches()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=5, help="timed runs per benchmark and language (default 5)")
    ap.add_argument("--warmup", type=int, default=0, help="untimed runs before the timed ones (default 0)")
    ap.add_argument("--only", help=f"comma-separated benchmarks ({', '.join(all_benches)})")
    ap.add_argument("--langs", help=f"comma-separated languages ({', '.join(LANGS)})")
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds before a single run is killed (default 60)")
    ap.add_argument("--build-timeout", type=float, default=300.0, help="seconds before a build is abandoned (default 300)")
    ap.add_argument("--scriptc-cache", action="store_true", help="let scriptc use its build cache (compile times become cache hits)")
    ap.add_argument("--no-json", action="store_true", help="don't write bench/results/<timestamp>.json")
    ap.add_argument("--list", action="store_true", help="list benchmarks and languages, then exit")
    a = ap.parse_args()

    if a.list:
        print("benchmarks:", ", ".join(all_benches))
        print("languages: ", ", ".join(LANGS))
        return
    if a.runs < 1:
        sys.exit("--runs must be >= 1")

    benches = parse_list(a.only, all_benches, "benchmark")
    langs = parse_list(a.langs, LANGS, "language")
    versions = toolchain_versions()
    started = dt.datetime.now()

    print(f"# Tov micro-benchmarks — {started:%Y-%m-%d %H:%M}", file=sys.stderr)
    print(f"{platform.platform()} · {platform.machine()} · runs={a.runs} warmup={a.warmup}", file=sys.stderr)

    results: list[Result] = []
    for bench in benches:
        # Always compute the C reference output, even when C isn't a selected language.
        expected = None
        ref_file = MICRO_DIR / bench / "expected.txt"
        c_result = None
        if "c" in langs:
            c_result = bench_one(bench, "c", a, None)
            if c_result.status == "ok":
                expected = c_result.stdout
        if expected is None and ref_file.exists():
            expected = ref_file.read_text()
        if c_result is not None and ref_file.exists() and c_result.status == "ok" and c_result.stdout != ref_file.read_text():
            c_result.status, c_result.reason = "mismatch", "C output differs from expected.txt"
        if c_result is not None:
            c_result.output_matches = None if expected is None else c_result.stdout == expected

        for lang in langs:
            if lang == "c":
                r = c_result
            else:
                r = bench_one(bench, lang, a, expected)
            assert r is not None
            results.append(r)
            shown = f"{fmt_ms(r.median_s)} ms" if r.median_s is not None else ""
            print(f"  {bench:<13} {lang:<8} {r.status:<12} {shown}", file=sys.stderr, flush=True)

    print()
    print(f"Median wall time over {a.runs} run(s), ratio vs C in parentheses. Node/Bun times include runtime startup and JIT warm-up.")
    print()
    print(summary_table(results, benches, langs))
    print()
    print(detail_table(results))
    issues = problems(results)
    if issues:
        print()
        print("Not measured / problems:")
        print()
        print(issues)

    if not a.no_json:
        RESULTS_DIR.mkdir(parents=True, exist_ok=True)
        path = RESULTS_DIR / f"{started:%Y%m%d-%H%M%S}.json"
        doc = {
            "started": started.isoformat(timespec="seconds"),
            "machine": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "processor": platform.processor(),
                "cpu_count": os.cpu_count(),
                "load_average": os.getloadavg() if hasattr(os, "getloadavg") else None,
            },
            "settings": {"runs": a.runs, "warmup": a.warmup, "timeout_s": a.timeout, "scriptc_cache": a.scriptc_cache},
            "toolchains": versions,
            "results": [r.__dict__ for r in results],
        }
        path.write_text(json.dumps(doc, indent=2) + "\n")
        print(f"\nwrote {path.relative_to(REPO_DIR)}", file=sys.stderr)


if __name__ == "__main__":
    main()
