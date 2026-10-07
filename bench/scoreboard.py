#!/usr/bin/env python3
"""Every benchmark, one table: each metric with Tov's number, the best of the others and who wins.

    bench/scoreboard.py [--only micro,http,fetch,npm,cap,build] [--quick]

Runs the benchmarks below (each interleaves its runtimes and writes bench/results/<name>-*.json),
reads what they wrote, and prints every metric with Tov, the best competitor, and WIN or LOSE
(with the margin). Tov is meant to win every row. Writes bench/results/scoreboard-<timestamp>.json.

  micro  bench/run.py: programs in Tov, C, Rust, Bun and Node (time, peak memory, binary size,
         compile time)
  http   bench/http/run.py: the HTTP server against Rust (axum) and Bun, one worker
  fetch  bench/fetch/run.py: fetch() against Bun, Node and Rust (reqwest)
  npm    bench/npm/run.py: zod from Tov against Bun (startup, calls, a service, binary, rebuild)
  cap    bench/cap-media/run.py: Cap's media server, unchanged, on Bun and Tov
  build  here: building Cap's media server with `tov build` against `bun build --compile` (with
         nothing cached, again with nothing changed, after a JavaScript edit) and the binaries'
         sizes

Timings on a busy machine are noise: the load average is printed, and kept in the JSON.
"""
import argparse, glob, json, os, shutil, statistics, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RESULTS = os.path.join(HERE, "results")
TOV = os.path.join(ROOT, "target", "release", "tov")


def newest(prefix, since):
    files = [f for f in glob.glob(os.path.join(RESULTS, prefix + "*.json")) if os.path.getmtime(f) >= since]
    if not files:
        raise RuntimeError(f"no bench/results/{prefix}*.json written by this run")
    return json.load(open(max(files, key=os.path.getmtime)))


def run(cmd, **kw):
    print("$ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


# a metric: (section, name, unit, "lower" or "higher" is better, {runtime: value})
rows = []


def add(section, name, unit, better, values):
    values = {k: v for k, v in values.items() if v is not None}
    if "tov" in values and len(values) > 1:
        rows.append((section, name, unit, better, values))


def micro(quick):
    t = time.time()
    run([sys.executable, os.path.join(HERE, "run.py"), "--runs", "3" if quick else "5", "--warmup", "1", "--langs", "tov,c,rust,bun,node"])
    d = newest("2", t)
    by = {}
    for r in d["results"]:
        if r.get("status") == "ok":
            by.setdefault(r["bench"], {})[r["lang"]] = r
    for bench, langs in sorted(by.items()):
        add("micro", f"{bench} time", "ms", "lower", {l: r["median_s"] * 1000 for l, r in langs.items()})
        add("micro", f"{bench} peak memory", "MB", "lower", {l: r["peak_rss_bytes"] / 1e6 for l, r in langs.items() if r.get("peak_rss_bytes")})
    any_bench = next(iter(by.values()), {})
    compiled = [l for l in ("tov", "c", "rust") if l in any_bench]
    add("micro", "binary size (stripped, median)", "KB", "lower",
        {l: statistics.median(by[b][l]["stripped_bytes"] for b in by if l in by[b] and by[b][l].get("stripped_bytes")) / 1024 for l in compiled})
    add("micro", "compile time (median)", "ms", "lower",
        {l: statistics.median(by[b][l]["compile_s"] for b in by if l in by[b] and by[b][l].get("compile_s")) * 1000 for l in compiled})


def http(quick):
    t = time.time()
    run([sys.executable, os.path.join(HERE, "http", "run.py"), "--workers", "1", "--secs", "3" if quick else "5", "--only", "tov,rust,bun"])
    d = newest("http-", t)
    by = {}
    for r in d["results"]:
        by.setdefault(r["route"], {})[r["server"]] = r
    for route, servers in by.items():
        add("http", f"{route} req/s", "req/s", "higher", {s: r["rps"] for s, r in servers.items()})
        add("http", f"{route} p99", "µs", "lower", {s: r["p99_us"] for s, r in servers.items()})
        add("http", f"{route} CPU per request", "µs", "lower", {s: r.get("cpu_us_per_req") for s, r in servers.items()})
        add("http", f"{route} memory", "MB", "lower", {s: r.get("pss_mb") or r.get("rss_mb") for s, r in servers.items()})


def fetch(quick):
    t = time.time()
    cmd = [sys.executable, os.path.join(HERE, "fetch", "run.py"), "--only", "tov,bun,node,rust,rust-mt"]
    if quick:
        cmd += ["--scale", "0.3"]
    run(cmd)
    d = newest("fetch-", t)
    for scen, clients in d["results"].items():
        # (a run that failed has no numbers: a client none of whose runs finished is left out)
        ok = {c: [r for r in runs if isinstance(r, dict) and "rps" in r] for c, runs in clients.items()}
        ok = {c: runs for c, runs in ok.items() if runs}
        med = lambda runs, k: statistics.median(x[k] for x in runs)
        add("fetch", f"{scen} req/s", "req/s", "higher", {c: med(runs, "rps") for c, runs in ok.items()})
        add("fetch", f"{scen} CPU", "s", "lower", {c: med(runs, "cpu") for c, runs in ok.items()})
        add("fetch", f"{scen} peak memory", "MB", "lower", {c: med(runs, "rss") / 1e6 for c, runs in ok.items()})


def npm(quick):
    t = time.time()
    run([sys.executable, os.path.join(HERE, "npm", "run.py"), "--repeat", "3"])
    d = newest("npm-", t)
    for name, v in d.items():
        if not isinstance(v, dict):
            continue
        better = "higher" if "req/s" in name else "lower"
        add("npm", name, "", better, v)


def cap(quick):
    t = time.time()
    run([sys.executable, os.path.join(HERE, "cap-media", "run.py"), "--reps", "3", "--secs", "6" if quick else "10"])
    d = newest("cap-media-", t)
    med = d["median"]
    higher = {"health_rps", "probe_ops", "extract_ops"}
    skip = {"probe_failed", "extract_failed"}
    for k in med.get("tov", {}):
        if k in skip:
            continue
        add("cap", k, "", "higher" if k in higher else "lower", {r: med[r].get(k) for r in med})
    for r in med:
        failed = (med[r].get("probe_failed") or 0) + (med[r].get("extract_failed") or 0)
        if failed:
            print(f"warning: {r} had failed requests ({failed})")


def build(quick):
    ms = os.path.join(HERE, "cap-media", "out", "ms")
    if not os.path.exists(os.path.join(ms, "main.tov")):
        print("build: run bench/cap-media/run.py once first (it sets up out/ms)")
        return
    engine = os.path.join(os.environ.get("TOV_CACHE_DIR", os.path.expanduser("~/.cache/tov")), "jsc", f"{'darwin' if sys.platform == 'darwin' else 'linux'}-{os.uname().machine.replace('aarch64', 'arm64')}")
    out = tempfile.mkdtemp(prefix="tov-scoreboard-")
    reps = 3

    def timed(cmd, env=None):
        t = time.perf_counter()
        subprocess.run(cmd, cwd=ms, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return (time.perf_counter() - t) * 1000

    cold, again, edit, bun = [], [], [], []
    src = os.path.join(ms, "src", "routes", "health.ts")
    text = open(src).read()
    try:
        for i in range(reps):
            cache = os.path.join(out, f"cache{i}")
            env = dict(os.environ, TOV_CACHE_DIR=cache)
            if os.path.isdir(engine):
                env["TOV_JSC_DIR"] = engine
            cold.append(timed([TOV, "build", "main.tov", "-o", os.path.join(out, "ms-tov")], env))
            again.append(timed([TOV, "build", "main.tov", "-o", os.path.join(out, "ms-tov")], env))
            with open(src, "w") as f:
                f.write(text + f"\nglobalThis.__scoreboardEdit = {i};\n")
            edit.append(timed([TOV, "build", "main.tov", "-o", os.path.join(out, "ms-tov")], env))
            with open(src, "w") as f:
                f.write(text)
            bun.append(timed(["bun", "build", "--compile", "src/index.ts", "--outfile", os.path.join(out, "ms-bun")]))
        add("build", "media server, nothing cached", "ms", "lower", {"tov": statistics.median(cold), "bun": statistics.median(bun)})
        add("build", "media server, nothing changed", "ms", "lower", {"tov": statistics.median(again), "bun": statistics.median(bun)})
        add("build", "media server, after a JavaScript edit", "ms", "lower", {"tov": statistics.median(edit), "bun": statistics.median(bun)})
        add("build", "media server binary", "MB", "lower", {"tov": os.path.getsize(os.path.join(out, "ms-tov")) / 1e6, "bun": os.path.getsize(os.path.join(out, "ms-bun")) / 1e6})
    finally:
        with open(src, "w") as f:
            f.write(text)
        shutil.rmtree(out, ignore_errors=True)


SECTIONS = {"micro": micro, "http": http, "fetch": fetch, "npm": npm, "cap": cap, "build": build}


def fmt(v):
    return f"{v:,.0f}" if abs(v) >= 100 else f"{v:,.2f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default=",".join(SECTIONS))
    ap.add_argument("--quick", action="store_true", help="fewer runs and shorter loads")
    a = ap.parse_args()
    load = os.getloadavg()
    print(f"load average {load[0]:.1f} {load[1]:.1f} {load[2]:.1f}" + ("  (busy: timings will be noisy)" if load[0] > 4 else ""), flush=True)
    for name in a.only.split(","):
        try:
            SECTIONS[name](a.quick)
        except Exception as e:
            print(f"{name}: failed: {e}", flush=True)
    wins = losses = 0
    print()
    print(f"{'':6} {'metric':44} {'Tov':>12} {'best other':>22}  result")
    out = []
    for section, name, unit, better, values in rows:
        tov = values["tov"]
        others = {k: v for k, v in values.items() if k != "tov" and not k.startswith("tov")}
        if not others:
            continue
        best_name = (min if better == "lower" else max)(others, key=others.get)
        best = others[best_name]
        win = tov <= best if better == "lower" else tov >= best
        margin = (best / tov - 1) * 100 if better == "lower" else (tov / best - 1) * 100 if best else 0
        wins += win
        losses += not win
        print(f"{section:6} {name + (f' ({unit})' if unit else ''):44} {fmt(tov):>12} {best_name + ' ' + fmt(best):>22}  {'WIN ' if win else 'LOSE'} {margin:+.0f}%")
        out.append({"section": section, "metric": name, "unit": unit, "better": better, "values": values, "win": win})
    print(f"\nTov wins {wins} of {wins + losses}" + (f"; loses {losses}" if losses else ""))
    os.makedirs(RESULTS, exist_ok=True)
    path = os.path.join(RESULTS, time.strftime("scoreboard-%Y%m%d-%H%M%S.json"))
    json.dump({"load": load, "rows": out}, open(path, "w"), indent=1)
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
