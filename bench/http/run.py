#!/usr/bin/env python3
"""HTTP server benchmark: Tov (std/http) vs Bun.serve vs node:http vs Rust (axum/tokio).

    bench/http/run.py [--workers 1,8] [--conns 64] [--secs 5] [--only tov,bun] [--pipeline 1]

Builds every server, then for each worker count and route starts one server at a time and
drives it with ./load (a wrk-style keep-alive generator in load.c). Prints req/s and latency
percentiles, and writes bench/results/http-<timestamp>.json.
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(HERE, "out")
PORT = 3217

ROUTES = [
    ("hello", "GET", "/", None),
    ("json", "GET", "/json", None),
    ("echo", "POST", "/echo", '{"id":1,"name":"Ada Lovelace","email":"ada@example.com","tags":["a","b","c"]}'),
]


def sh(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build():
    os.makedirs(OUT, exist_ok=True)
    sh(["cc", "-O2", "-o", os.path.join(OUT, "load"), os.path.join(HERE, "load.c"), "-lpthread"])
    sh(["cargo", "build", "--release", "-q", "--manifest-path", os.path.join(ROOT, "Cargo.toml")])
    sh([os.path.join(ROOT, "target/release/tov"), "build", os.path.join(HERE, "server.tov.ts"), "-o", os.path.join(OUT, "tov-server")], stdout=subprocess.DEVNULL)
    sh(["cargo", "build", "--release", "-q", "--locked", "--manifest-path", os.path.join(HERE, "rust/Cargo.toml")])


# In the Linux container (linux.sh) the binaries are prebuilt into $BIN_DIR.
BIN = os.environ.get("BIN_DIR")

SERVERS = {
    "tov": lambda: [os.path.join(BIN or OUT, "tov-server")],
    "rust": lambda: [os.path.join(BIN, "rust-server") if BIN else os.path.join(HERE, "rust/target/release/server")],
    "bun": lambda: [os.path.join(BIN, "bun") if BIN else "bun", os.path.join(HERE, "server.bun.ts")],
    "node": lambda: ["node", os.path.join(HERE, "server.node.mjs")],
}
SERVERS["rust-tpc"] = SERVERS["rust"]  # with MODE=tpc (Linux: SO_REUSEPORT balances)
# Rust with TCP_NODELAY (NODELAY=1; thread-per-core on Linux): responses go out at once, as Tov
# sends them. axum's default keeps Nagle's algorithm on, which holds a response while earlier
# data is unacknowledged: on loopback that moves the cost of sending onto the client, and the
# wait shows up as latency.
SERVERS["rust-nodelay"] = SERVERS["rust"]


def wait_port(port, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def cpu_seconds(pgid):
    """Total CPU time (user+sys) of the server's process group."""
    pids = subprocess.run(["pgrep", "-g", str(pgid)], capture_output=True, text=True).stdout.split()
    if not pids:
        return 0.0
    if os.path.exists("/proc/self/stat"):  # Linux: clock ticks (procps `ps` shows whole seconds)
        tick, total = os.sysconf("SC_CLK_TCK"), 0.0
        for pid in pids:
            try:
                fields = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
                total += (int(fields[11]) + int(fields[12])) / tick
            except OSError:
                pass
        return total
    out = subprocess.run(["ps", "-o", "time=", "-p", ",".join(pids)], capture_output=True, text=True).stdout
    total = 0.0
    for t in out.split():
        parts = [float(x) for x in t.replace("-", ":").split(":")]
        total += sum(v * 60 ** i for i, v in enumerate(reversed(parts)))
    return total


def pss_mb(pgid):
    """Proportional set size of the process group (MB): pages shared between processes (a
    forked worker's code and libraries) are split between them. Linux only; else RSS."""
    pids = subprocess.run(["pgrep", "-g", str(pgid)], capture_output=True, text=True).stdout.split()
    total, ok = 0, False
    for pid in pids:
        try:
            for line in open(f"/proc/{pid}/smaps_rollup"):
                if line.startswith("Pss:"):
                    total += int(line.split()[1])
                    ok = True
        except OSError:
            pass
    if ok:
        return total / 1024
    # macOS: the sum of physical footprints (a process's own dirty memory; like PSS, a forked
    # worker's shared code isn't counted again)
    if pids and shutil.which("footprint"):
        out = subprocess.run(["footprint", "-f", "bytes"] + [a for p in pids for a in ("-p", p)], capture_output=True, text=True).stdout
        # Several processes: per-process lines, then their sum as "Summary Footprint".
        total = re.search(r"Summary Footprint: (\d+) B", out)
        if total:
            return int(total.group(1)) / (1 << 20)
        fp = [int(m.group(1)) for m in re.finditer(r"Footprint: (\d+) B", out)]
        if fp:
            return sum(fp) / (1 << 20)
    return rss_mb(pgid)


def rss_mb(pgid):
    """Resident memory of the server's process group (MB)."""
    pids = subprocess.run(["pgrep", "-g", str(pgid)], capture_output=True, text=True).stdout.split()
    if not pids:
        return 0.0
    out = subprocess.run(["ps", "-o", "rss=", "-p", ",".join(pids)], capture_output=True, text=True).stdout
    return sum(int(x) for x in out.split()) / 1024


RATE = 0  # --rate: open loop at this many requests/s (latency at a fixed load)


def load(route, conns, threads, secs, pipeline, pgid):
    _, method, path, body = route
    warm = 1.0
    cmd = [os.path.join(OUT, "load"), "-j", "-c", str(conns), "-t", str(threads), "-d", str(secs), "-w", str(warm), "-P", str(pipeline), "-m", method]
    if body:
        cmd += ["-b", body]
    if RATE:
        cmd += ["-R", str(RATE)]
    cmd.append(f"http://127.0.0.1:{PORT}{path}")
    if BIN:
        cmd[0] = os.path.join(BIN, "load")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
    time.sleep(warm + 0.1)
    c0 = cpu_seconds(pgid)
    time.sleep(secs - 0.2)
    c1 = cpu_seconds(pgid)
    mem, pss = rss_mb(pgid), pss_mb(pgid)
    r = json.loads(p.communicate()[0])
    r["rss_mb"] = mem
    r["pss_mb"] = pss
    # CPU the server spent per request (µs), over the (secs - 0.2) sampling window.
    r["cpu_us_per_req"] = (c1 - c0) * 1e6 / max(1.0, r["rps"] * (secs - 0.2))
    r["cpu_cores"] = (c1 - c0) / (secs - 0.2)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workers", default="1,8")
    ap.add_argument("--conns", type=int, default=64)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--secs", type=float, default=5)
    ap.add_argument("--pipeline", type=int, default=1)
    ap.add_argument("--rate", type=int, default=0, help="open loop: fixed requests/s (compare latency at equal load)")
    ap.add_argument("--repeat", type=int, default=3, help="repetitions (servers interleaved); the median is reported")
    # rust-tpc needs SO_REUSEPORT load balancing, which only Linux has.
    default = [n for n in SERVERS if n != "rust-tpc" or os.uname().sysname == "Linux"]
    ap.add_argument("--only", default=",".join(default))
    ap.add_argument("--routes", default=",".join(r[0] for r in ROUTES))
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args()
    global RATE
    RATE = a.rate
    if not a.no_build:
        build()
    names = a.only.split(",")
    routes = [r for r in ROUTES if r[0] in a.routes.split(",")]
    results = []
    for workers in [int(w) for w in a.workers.split(",")]:
        # Servers are interleaved within each repetition, so a noisy phase on the machine
        # lands on every server rather than on one; the median run per route is reported.
        samples = {}
        for rep in range(a.repeat):
            for name in names:
                linux = os.uname().sysname == "Linux"
                tpc = name == "rust-tpc" or (name == "rust-nodelay" and linux)
                env = dict(os.environ, PORT=str(PORT), WORKERS=str(workers), MODE="tpc" if tpc else "", NODELAY="1" if name == "rust-nodelay" else "")
                proc = subprocess.Popen(SERVERS[name](), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                try:
                    wait_port(PORT)
                    time.sleep(0.3 if workers == 1 else 1.0)
                    for route in routes:
                        samples.setdefault((name, route[0]), []).append(load(route, a.conns, a.threads, a.secs, a.pipeline, proc.pid))
                finally:
                    os.killpg(proc.pid, 15)
                    proc.wait()
                    time.sleep(0.5)
            print(f"  [{workers} workers] repetition {rep + 1}/{a.repeat} done", file=sys.stderr, flush=True)
        mode = f"open loop at {a.rate:,} req/s" if a.rate else f"pipeline {a.pipeline}"
        print(f"\n== {workers} worker{'s' if workers > 1 else ''}, {a.conns} connections, {mode} ==")
        print(f"{'server':<8} {'route':<6} {'req/s':>10} {'avg':>8} {'p50':>8} {'p99':>8} {'p99.9':>8} {'cores':>6} {'cpu/req':>8} {'rss':>7} {'mem':>7}")
        for route in routes:
            for name in names:
                runs = sorted(samples[(name, route[0])], key=lambda x: x["rps"])
                r = dict(runs[len(runs) // 2])
                r["rps_all"] = [round(x["rps"]) for x in runs]
                r.update(server=name, route=route[0], workers=workers, conns=a.conns, pipeline=a.pipeline, rate=a.rate)
                results.append(r)
                print(f"{name:<8} {route[0]:<6} {r['rps']:>10,.0f} {r['avg_us']:>6.0f}us {r['p50_us']:>6}us {r['p99_us']:>6}us {r['p999_us']:>6}us {r['cpu_cores']:>6.2f} {r['cpu_us_per_req']:>6.2f}us {r['rss_mb']:>5.1f}MB {r['pss_mb']:>5.1f}MB", flush=True)
    os.makedirs(os.path.join(HERE, "../results"), exist_ok=True)
    tag = "-linux" if os.uname().sysname == "Linux" else ""
    path = os.path.join(HERE, "../results", time.strftime(f"http{tag}-%Y%m%d-%H%M%S.json"))
    with open(path, "w") as f:
        json.dump({"machine": f"{os.uname().sysname} {os.uname().machine}", "results": results}, f, indent=1)
    print(f"\nwrote {os.path.relpath(path, ROOT)}")


if __name__ == "__main__":
    sys.exit(main())
