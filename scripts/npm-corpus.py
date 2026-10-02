#!/usr/bin/env python3
"""npm compatibility corpus: loads every top-level package in a project's node_modules with Barm
(its bundler, on its runtime: JavaScriptCore with runtime/js.c and node.c) and with Bun, and
compares what each exports (typeof, and the sorted keys).

  scripts/npm-corpus.py <project-dir> [name-filter] [--jobs N] [--limit N]

Packages Bun itself can't load are skipped. Results: target/npm-corpus/results.json (per package:
name, status, detail) and a summary by status and by cause. macOS (JavaScriptCore) and Bun needed.
"""
import argparse, collections, concurrent.futures as cf, json, os, re, subprocess, sys, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "target", "npm-corpus")
BARM = os.path.join(ROOT, "target", "release", "barm")
RUNNER = os.path.join(OUT, "runner")

PROBE = """
var __log = typeof __out === "function" ? __out : function (s) { console.log(s); };
var __m; try { __m = (%s)(%s); } catch (e) { __log("THROW " + (e && e.message ? e.message.split("\\n")[0] : String(e))); __m = undefined; }
if (__m !== undefined) {
  var __k = []; try { for (var k in __m) __k.push(k); __k = __k.concat(Object.getOwnPropertyNames(__m).filter(function (k) { return __k.indexOf(k) < 0 && k !== "prototype" && k !== "length" && k !== "name" && k !== "caller" && k !== "arguments"; })); } catch (e) {}
  __log("TYPE " + typeof __m);
  __log("KEYS " + __k.filter(function (k) { return k !== "__esModule" && k !== "default" && k !== "module.exports"; }).sort().slice(0, 60).join(","));
}
"""


def sh(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build():
    os.makedirs(OUT, exist_ok=True)
    sh(["cargo", "build", "-q", "--release"], cwd=ROOT)
    rt = os.path.join(ROOT, "runtime")
    srcs = [os.path.join(ROOT, "scripts", "npm-corpus-runner.c")] + [os.path.join(rt, f) for f in ("js.c", "node.c", "napi.c", "barm.c")]
    sh(["cc", "-O1", "-std=gnu11", "-w", "-I", rt, *srcs, "-framework", "JavaScriptCore", "-framework", "CoreFoundation", "-o", RUNNER])
    plist = os.path.join(OUT, "jit.plist")
    with open(plist, "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>com.apple.security.cs.allow-jit</key><true/></dict></plist>\n')
    sh(["codesign", "-s", "-", "-f", "--entitlements", plist, RUNNER], stderr=subprocess.DEVNULL)


def packages(project, flt, limit):
    nm = os.path.join(project, "node_modules")
    out = []
    for name in sorted(os.listdir(nm)):
        if name.startswith(".") or name == "@types":
            continue
        if name.startswith("@"):
            out += [f"{name}/{sub}" for sub in sorted(os.listdir(os.path.join(nm, name)))]
        else:
            out.append(name)
    out = [p for p in out if os.path.isfile(os.path.join(nm, p, "package.json")) and flt in p]
    return out[:limit]


def run(cmd, cwd=None, timeout=30):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=cwd)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -9, "", "timeout"


_slot = threading.local()
_workers = iter(range(10**6))
_workers_lock = threading.Lock()


def scratch(project):
    """A directory whose node_modules is the project's (Bun resolves from the script's directory)."""
    import hashlib
    d = os.path.join(OUT, "p" + hashlib.sha1(project.encode()).hexdigest()[:12])
    os.makedirs(d, exist_ok=True)
    link = os.path.join(d, "node_modules")
    if not os.path.islink(link):
        os.symlink(os.path.join(project, "node_modules"), link)
    return d


def check(project, pkg):
    # scratch files per worker, overwritten: bounded disk use
    if not hasattr(_slot, "n"):
        with _workers_lock:
            _slot.n = next(_workers)
    base = os.path.join(scratch(project), f"w{_slot.n}")
    with open(base + ".bun.js", "w") as f:
        f.write(PROBE % ("require", json.dumps(pkg)))
    _, bout, berr = run(["bun", "--no-install", base + ".bun.js"])
    if "THROW" in bout or "TYPE" not in bout:
        return pkg, "bun-throws", (bout or berr).strip()[:300]
    code, _, err = run([BARM, "__bundle", project, pkg, "--blob", "-o", base + ".blob"])
    if code != 0:
        return pkg, "bundle-error", (err.strip().splitlines() or ["?"])[-1][:300]
    with open(base + ".probe.js", "w") as f:
        f.write(PROBE % ("__barm_npm", json.dumps(pkg)))
    code, out, err = run([RUNNER, base + ".blob", base + ".probe.js"])
    if code != 0 and not out:
        return pkg, "crash", ((err or out).strip().splitlines() or ["?"])[-1][:300]
    if out.strip() == bout.strip():
        return pkg, "ok", ""
    if "THROW" in out:
        return pkg, "throws", [l for l in out.splitlines() if l.startswith("THROW")][0][:300]
    return pkg, "differs", f"barm: {out.strip()[:200]} | bun: {bout.strip()[:200]}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("project")
    ap.add_argument("filter", nargs="?", default="")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--limit", type=int, default=10**9)
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args()
    if not a.no_build:
        build()
    project = os.path.abspath(a.project)
    pkgs = packages(project, a.filter, a.limit)
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        results = list(ex.map(lambda p: check(project, p), pkgs))
    with open(os.path.join(OUT, "results.json"), "w") as f:
        json.dump([{"package": p, "status": s, "detail": d} for p, s, d in results], f, indent=1)
    by = collections.Counter(s for _, s, _ in results)
    loadable = len(results) - by["bun-throws"]
    print(f"{len(results)} packages ({loadable} that Bun loads): " + ", ".join(f"{s} {n}" for s, n in by.most_common()))
    if loadable:
        print(f"same as Bun: {by['ok'] / loadable:.1%}")
    causes = collections.Counter()
    example = {}
    for p, s, d in results:
        if s in ("throws", "bundle-error", "crash"):
            k = re.sub(r"\(from [^)]*\)", "", d)[:120]
            causes[k] += 1
            example.setdefault(k, p)
    for k, n in causes.most_common(40):
        print(f"  {n:4d}  {k}   (e.g. {example[k]})")


if __name__ == "__main__":
    main()
