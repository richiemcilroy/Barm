#!/usr/bin/env python3
"""fetch() client benchmark: Tov vs Bun vs Node (undici) vs Rust (reqwest on hyper).

    bench/fetch/run.py [--only tov,bun] [--scenarios hello-1,hello-64] [--reps 3] [--scale 1]

Every client does the same work (client.tov, run by Tov natively and by Bun and Node as
TypeScript; rust-client/ for Rust) against one server (server/: axum on a 4-thread tokio
runtime, so the server is never the limit). For each scenario (a route, and how many request
loops run at once) it reports requests/s, the client's CPU time per request (user + system)
and its peak RSS: the median of --reps runs, with clients interleaved. Writes
bench/results/fetch-<timestamp>.json.
"""
import argparse, json, os, platform, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(HERE, "out")
PORT = 3311
TLS_PORT = 3312
BASE = f"http://127.0.0.1:{PORT}"
# HTTPS: the server's certificate (tests/fetch/tls) is for localhost, signed by the test CA,
# which every client trusts through NODE_EXTRA_CA_CERTS
TLS_BASE = f"https://localhost:{TLS_PORT}"
CA = os.path.join(ROOT, "tests/fetch/tls/ca.pem")

# (name, route, concurrent loops, requests, over TLS)
SCENARIOS = [
    ("hello-1", "hello", 1, 20000, False),
    ("hello-64", "hello", 64, 100000, False),
    ("json-64", "json", 64, 100000, False),
    ("echo-64", "echo", 64, 100000, False),
    ("big-1", "big", 1, 100, False),
    ("big-8", "big", 8, 200, False),
    ("gzip-1", "gzip", 1, 100, False),
    ("tls-hello-1", "hello", 1, 20000, True),
    ("tls-hello-64", "hello", 64, 100000, True),
    ("tls-new-1", "close", 1, 2000, True),
    ("tls-big-1", "big", 1, 100, True),
]


def sh(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build():
    os.makedirs(OUT, exist_ok=True)
    sh(["cargo", "build", "--release", "-q", "--manifest-path", os.path.join(ROOT, "Cargo.toml")])
    sh(["cargo", "build", "--release", "-q", "--offline", "--manifest-path", os.path.join(HERE, "server/Cargo.toml")])
    sh(["cargo", "build", "--release", "-q", "--offline", "--manifest-path", os.path.join(HERE, "rust-client/Cargo.toml")])
    sh([os.path.join(ROOT, "target/release/tov"), "build", os.path.join(HERE, "client.tov"), "-o", os.path.join(OUT, "tov-client")], stdout=subprocess.DEVNULL)
    # Bun and Node run the same file as TypeScript, without Tov's `try` markers and `throws`.
    text = open(os.path.join(HERE, "client.tov")).read()
    import re
    text = re.sub(r"\btry (?=await|new |[A-Za-z_]+\()", "", text)
    text = re.sub(r"\s+throws\s+[A-Za-z]+(?=\s*\{)", "", text)
    open(os.path.join(OUT, "client.ts"), "w").write(text)


CLIENTS = {
    "tov": [os.path.join(OUT, "tov-client")],
    "bun": ["bun", os.path.join(OUT, "client.ts")],
    "node": ["node", "--no-warnings", os.path.join(OUT, "client.ts")],
    "rust": [os.path.join(HERE, "rust-client/target/release/fetch-bench-client")],
    "rust-mt": [os.path.join(HERE, "rust-client/target/release/fetch-bench-client")],
}


def run_client(argv, env, timeout):
    """Wall seconds, CPU seconds, peak RSS (bytes) and output of one run."""
    t0 = time.perf_counter()
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    deadline = time.time() + timeout
    while True:
        pid, status, ru = os.wait4(p.pid, os.WNOHANG)
        if pid:
            break
        if time.time() > deadline:
            p.kill()
            os.wait4(p.pid, 0)
            raise RuntimeError("timed out")
        time.sleep(0.002)
    wall = time.perf_counter() - t0
    out = p.stdout.read().decode()
    if status != 0:
        raise RuntimeError(f"exit {status}: {p.stderr.read().decode()[-300:]}")
    rss = ru.ru_maxrss if sys.platform == "darwin" else ru.ru_maxrss * 1024
    return wall, ru.ru_utime + ru.ru_stime, rss, out.strip()


def wait_port(port):
    for _ in range(200):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="tov,bun,node,rust,rust-mt")
    ap.add_argument("--scenarios", default=",".join(s[0] for s in SCENARIOS))
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--scale", type=float, default=1.0, help="multiply every request count")
    ap.add_argument("--timeout", type=float, default=120)
    a = ap.parse_args()
    clients = a.only.split(",")
    scen = [(name, mode, conc, max(conc, int(n * a.scale)), tls) for name, mode, conc, n, tls in SCENARIOS if name in a.scenarios.split(",")]
    build()
    server = subprocess.Popen([os.path.join(HERE, "server/target/release/fetch-bench-server")], env={**os.environ, "PORT": str(PORT), "TLS_PORT": str(TLS_PORT), "WORKERS": "4"})
    results = {}
    try:
        wait_port(PORT)
        wait_port(TLS_PORT)
        for name, mode, conc, n, tls in scen:
            for rep in range(a.reps):
                for c in clients:
                    env = {**os.environ, "MODE_RT": "mt" if c == "rust-mt" else "ct", "NODE_EXTRA_CA_CERTS": CA}
                    try:
                        wall, cpu, rss, out = run_client(CLIENTS[c] + [mode, TLS_BASE if tls else BASE, str(n), str(conc)], env, a.timeout)
                        r = {"wall": wall, "cpu": cpu, "rss": rss, "rps": n / wall, "out": out}
                    except RuntimeError as e:
                        r = {"error": str(e)}
                    results.setdefault(name, {}).setdefault(c, []).append(r)
                    shown = f"{r['rps']:>9,.0f} req/s  {r['cpu'] / n * 1e6:6.1f} µs cpu/req  {r['rss'] / 1e6:6.1f} MB" if "rps" in r else r["error"]
                    print(f"  {name:9} {c:8} {rep + 1}: {shown}", flush=True)
    finally:
        server.kill()
        server.wait()
    med = lambda xs: sorted(xs)[len(xs) // 2]
    print("\n| scenario | " + " | ".join(clients) + " |")
    print("|---|" + "---:|" * len(clients))
    for name, mode, conc, n, tls in scen:
        for metric in ("rps", "cpu", "rss"):
            row = []
            for c in clients:
                ok = [r for r in results[name][c] if "rps" in r]
                if not ok:
                    row.append("failed")
                    continue
                v = med([r[metric] for r in ok])
                row.append(f"{v / 1000:,.1f}k/s" if metric == "rps" else f"{v / n * 1e6:.1f} µs" if metric == "cpu" else f"{v / 1e6:.1f} MB")
            label = {"rps": f"{name} ({n:,} requests)", "cpu": "cpu/request", "rss": "peak RSS"}[metric]
            print(f"| {label} | " + " | ".join(row) + " |")
    os.makedirs(os.path.join(ROOT, "bench/results"), exist_ok=True)
    path = os.path.join(ROOT, "bench/results", time.strftime("fetch-%Y%m%d-%H%M%S.json"))
    json.dump({"platform": platform.platform(), "machine": platform.machine(), "scenarios": scen, "results": results}, open(path, "w"), indent=1)
    print(f"\nwrote {path}")


if __name__ == "__main__":
    main()
