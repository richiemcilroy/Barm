#!/usr/bin/env python3
"""Cap's media server (Cap/apps/media-server) on Bun, as Cap runs it, and on Barm, unchanged.

    bench/cap-media/run.py --cap ~/github/Cap [--reps 3] [--secs 10] [--only bun,barm]

Copies the media server's package.json, bun.lock and src into out/ms, installs its packages
with `bun install --production`, and builds the Barm binary from out/ms/main.barm (which serves
the server's default export, as Bun does with an entry point's). Serves a generated 10 s 720p
H.264/AAC test video from a local origin (origin.mjs, under Node, with ranges), then for each runtime in turn (interleaved over --reps):
startup to the first /health answer, idle memory, /health under load, /video/probe (node-av,
over HTTP) and /audio/extract (an ffmpeg subprocess, streamed) with requests in flight, the
memory after, and shutdown on SIGTERM. Prints medians and writes
bench/results/cap-media-<timestamp>.json.
"""
import argparse, json, os, re, shutil, signal, socket, statistics, subprocess, sys, threading, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(HERE, "out")
MS = os.path.join(OUT, "ms")
PORT = 37461
MEDIA_PORT = 37490
SECRET = "bench-secret"
VIDEO = f"http://127.0.0.1:{MEDIA_PORT}/test.mp4"

MAIN_BARM = """import server from "cap-media-server"
import { serve } from "bun"

function main() {
  // Cap's media server, as Bun runs it: its default export served
  try serve(server)
}
"""


def sh(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def setup(cap):
    src = os.path.join(cap, "apps/media-server")
    os.makedirs(MS, exist_ok=True)
    for name in ["package.json", "bun.lock", "tsconfig.json"]:
        shutil.copy(os.path.join(src, name), MS)
    if os.path.exists(os.path.join(MS, "src")):
        shutil.rmtree(os.path.join(MS, "src"))
    shutil.copytree(os.path.join(src, "src"), os.path.join(MS, "src"), ignore=shutil.ignore_patterns("__tests__"))
    sh(["bun", "install", "--production"], cwd=MS, stdout=subprocess.DEVNULL)
    pkg = os.path.join(MS, "node_modules/cap-media-server")
    os.makedirs(pkg, exist_ok=True)
    with open(os.path.join(pkg, "package.json"), "w") as f:
        f.write('{"name":"cap-media-server","type":"module","main":"index.js"}\n')
    with open(os.path.join(pkg, "index.js"), "w") as f:
        f.write('export { default } from "../../src/index.ts";\n')
    with open(os.path.join(MS, "main.barm"), "w") as f:
        f.write(MAIN_BARM)
    sh(["cc", "-O2", "-o", os.path.join(OUT, "load"), os.path.join(ROOT, "bench/http/load.c"), "-lpthread"])
    sh(["cargo", "build", "--release", "-q", "--manifest-path", os.path.join(ROOT, "Cargo.toml")])
    t = time.time()
    sh([os.path.join(ROOT, "target/release/barm"), "build", "main.barm", "-o", "ms-barm"], cwd=MS, stdout=subprocess.DEVNULL)
    build_s = time.time() - t
    video = os.path.join(OUT, "test.mp4")
    if not os.path.exists(video):
        sh(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30", "-f", "lavfi", "-i",
            "sine=frequency=440:sample_rate=48000", "-t", "10", "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-shortest", "-movflags", "+faststart", video])
    return build_s


def serve_media():
    proc = subprocess.Popen(["node", os.path.join(HERE, "origin.mjs"), OUT, str(MEDIA_PORT)])
    for _ in range(200):
        if not port_free(MEDIA_PORT):
            return proc
        time.sleep(0.02)
    raise RuntimeError("the media origin didn't start")


RUNTIMES = {
    "bun": lambda: ["bun", "src/index.ts"],
    "barm": lambda: ["./ms-barm"],
}


def footprint(pid, peak=False):
    """The process's physical footprint in MB (macOS: what it costs the machine)."""
    out = subprocess.run(["vmmap", "-summary", str(pid)], capture_output=True, text=True).stdout
    m = re.search(r"Physical footprint \(peak\):\s+([\d.]+)([KMG])" if peak else r"Physical footprint:\s+([\d.]+)([KMG])", out)
    if not m:
        return None
    return float(m.group(1)) * {"K": 1 / 1024, "M": 1, "G": 1024}[m.group(2)]


# (no proxies: on macOS urllib would use the system's)
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def get(path, timeout=1):
    with OPENER.open(f"http://127.0.0.1:{PORT}{path}", timeout=timeout) as r:
        return r.status, r.read()


def client(route, body, conns, secs):
    out = subprocess.run(["node", os.path.join(HERE, "client.mjs"), f"http://127.0.0.1:{PORT}{route}", json.dumps(body), SECRET, str(conns), str(secs)],
                         capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def port_free(port):
    s = socket.socket()
    # (as servers bind: closed connections in TIME_WAIT don't hold the port)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def run_once(name, secs):
    if not port_free(PORT):
        raise RuntimeError(f"port {PORT} is in use")
    env = dict(os.environ, PORT=str(PORT), MEDIA_SERVER_WEBHOOK_SECRET=SECRET, NODE_ENV="production")
    t0 = time.time()
    log = open(os.path.join(OUT, f"{name}.log"), "w")
    proc = subprocess.Popen(RUNTIMES[name](), cwd=MS, env=env, stdout=log, stderr=subprocess.STDOUT)
    while True:
        try:
            if get("/health", 0.5)[0] == 200:
                break
        except Exception:
            if proc.poll() is not None:
                raise RuntimeError(f"{name} exited at startup")
            time.sleep(0.005)
    r = {"startup_ms": (time.time() - t0) * 1000}
    time.sleep(3)
    r["idle_mb"] = footprint(proc.pid)
    for attempt in range(2):
        load = subprocess.run([os.path.join(OUT, "load"), "-c", "64", "-d", str(secs), "-j", f"http://127.0.0.1:{PORT}/health"],
                              capture_output=True, text=True)
        if load.returncode == 0:
            break
        print(f"  ({name}: load failed: {load.stderr.strip()}; again)", flush=True)
        time.sleep(1)
    h = json.loads(load.stdout)
    r["health_rps"], r["health_p50_us"], r["health_p99_us"] = h["rps"], h["p50_us"], h["p99_us"]
    p = client("/video/probe", {"videoUrl": VIDEO}, 4, secs)
    r["probe_ops"], r["probe_p50_ms"], r["probe_p99_ms"], r["probe_failed"] = p["ops"], p["p50"], p["p99"], p["failed"]
    a = client("/audio/extract", {"videoUrl": VIDEO}, 2, secs)
    r["extract_ops"], r["extract_p50_ms"], r["extract_p99_ms"], r["extract_failed"] = a["ops"], a["p50"], a["p99"], a["failed"]
    time.sleep(3)
    r["after_mb"] = footprint(proc.pid)
    r["peak_mb"] = footprint(proc.pid, peak=True)
    t = time.time()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=30)
    r["shutdown_ms"] = (time.time() - t) * 1000
    log.close()
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cap", default=os.environ.get("CAP_DIR", os.path.expanduser("~/Documents/github/Cap")))
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--secs", type=int, default=10)
    ap.add_argument("--only", default="bun,barm")
    args = ap.parse_args()
    names = args.only.split(",")
    build_s = setup(args.cap)
    origin = serve_media()
    # (a first launch isn't timed: macOS checks a newly built binary's signature then)
    for n in names:
        env = dict(os.environ, PORT=str(PORT), MEDIA_SERVER_WEBHOOK_SECRET=SECRET)
        p = subprocess.Popen(RUNTIMES[n](), cwd=MS, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(3000):
            try:
                if get("/health", 0.5)[0] == 200:
                    break
            except Exception:
                time.sleep(0.01)
        # (idle a moment, as a deployment's first start does: Barm writes its bytecode cache and
        # Bun its transpiler cache then)
        time.sleep(2)
        p.send_signal(signal.SIGTERM)
        p.wait(timeout=30)
    runs = {n: [] for n in names}
    for rep in range(args.reps):
        for n in names:
            r = run_once(n, args.secs)
            runs[n].append(r)
            print(f"rep {rep + 1} {n}: " + " ".join(f"{k}={v:.1f}" if isinstance(v, float) else f"{k}={v}" for k, v in r.items()), flush=True)
    med = {n: {k: (statistics.median(v) if (v := [r[k] for r in runs[n] if r[k] is not None]) else None) for k in runs[n][0]} for n in names}
    print()
    print(f"{'':16}" + "".join(f"{n:>12}" for n in names))
    for k in med[names[0]]:
        print(f"{k:16}" + "".join(f"{med[n][k]:>12.1f}" if med[n][k] is not None else f"{'-':>12}" for n in names))
    print(f"\nbarm build: {build_s:.1f} s, binary {os.path.getsize(os.path.join(MS, 'ms-barm')) / 1e6:.1f} MB")
    os.makedirs(os.path.join(ROOT, "bench/results"), exist_ok=True)
    path = os.path.join(ROOT, "bench/results", time.strftime("cap-media-%Y%m%d-%H%M%S.json"))
    with open(path, "w") as f:
        json.dump({"runs": runs, "median": med, "barm_build_s": build_s,
                   "bun": subprocess.run(["bun", "--version"], capture_output=True, text=True).stdout.strip()}, f, indent=1)
    print(f"wrote {path}")
    origin.terminate()


if __name__ == "__main__":
    main()
