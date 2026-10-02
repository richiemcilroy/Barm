// Drives one media-server route with `conns` requests in flight for `secs` seconds (a closed loop:
// each worker sends its next request when the last one's body has arrived). Runs under Node, so
// the client is the same whichever runtime serves. Prints one JSON object.
//
//   node client.mjs <url> <json body> <secret> <conns> <secs>

const [, , url, body, secret, conns = "4", secs = "10"] = process.argv;
const end = performance.now() + Number(secs) * 1000;
const latencies = [];
let ok = 0;
let failed = 0;
let bytes = 0;

async function worker() {
	while (performance.now() < end) {
		const start = performance.now();
		try {
			const res = await fetch(url, {
				method: "POST",
				headers: { "content-type": "application/json", "x-media-server-secret": secret },
				body,
			});
			const data = await res.arrayBuffer();
			bytes += data.byteLength;
			if (res.status === 200) ok++;
			else failed++;
		} catch {
			failed++;
		}
		latencies.push(performance.now() - start);
	}
}

const t0 = performance.now();
await Promise.all(Array.from({ length: Number(conns) }, worker));
const elapsed = (performance.now() - t0) / 1000;
latencies.sort((a, b) => a - b);
const pct = (p) => latencies.length ? latencies[Math.min(latencies.length - 1, Math.floor(p * latencies.length))] : 0;
console.log(JSON.stringify({
	ops: ok / elapsed,
	ok,
	failed,
	bytes,
	p50: pct(0.5),
	p95: pct(0.95),
	p99: pct(0.99),
}));
