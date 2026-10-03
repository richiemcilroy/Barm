#!/usr/bin/env python3
"""Summarizes a JavaScript profile from an npm program run with BARM_JS_PROFILE=<file>
(JavaScriptCore's sampling profiler, one stack trace a millisecond).

    scripts/js-profile.py <file> [--top 40]

Prints the functions most often at the top of the stack (self) and anywhere on it (inclusive),
by name, source and line (the profiler names sources by number; see the frames' `sourceID`)."""
import argparse, collections, json

ap = argparse.ArgumentParser()
ap.add_argument("file")
ap.add_argument("--top", type=int, default=40)
args = ap.parse_args()

data = json.load(open(args.file))
traces = data["traces"]
self_ = collections.Counter()
incl = collections.Counter()
tiers = collections.Counter()


def key(f):
    name = f["name"] or "(anonymous)"
    if f["line"] == 4294967295:
        return f"{name} [{f['category']}]"
    return f"{name} {f['sourceID']}:{f['line']}"


for t in traces:
    frames = t["frames"]
    if not frames:
        continue
    self_[key(frames[0])] += 1
    tiers[frames[0]["category"]] += 1
    for k in set(key(f) for f in frames):
        incl[k] += 1

n = len(traces)
print(f"{n} samples ({n * data.get('interval', 0.001):.2f} s); top-frame tiers: " + ", ".join(f"{k} {v}" for k, v in tiers.most_common()))
print("\nself")
for k, v in self_.most_common(args.top):
    print(f"{100 * v / n:6.1f}%  {k}")
print("\ninclusive")
for k, v in incl.most_common(args.top):
    print(f"{100 * v / n:6.1f}%  {k}")
