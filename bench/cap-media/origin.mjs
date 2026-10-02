// The media origin: serves out/ (the test video) with Range support, as object storage does.
// Runs under Node, so it's the same whichever runtime the media server is on, with a worker per
// core: media readers cancel ranges partway, so each read can be a new connection, and one
// process can't accept them as fast as they come.
//
//   node origin.mjs <dir> <port>
import cluster from "node:cluster";
import { availableParallelism } from "node:os";
import { createServer } from "node:http";
import { createReadStream, statSync } from "node:fs";
import { join, normalize } from "node:path";

const [, , dir, port] = process.argv;

// (each worker accepts its own connections: the primary handing them out would be the bottleneck)
cluster.schedulingPolicy = cluster.SCHED_NONE;
if (cluster.isPrimary) {
	for (let i = 0; i < Math.min(8, availableParallelism()); i++) cluster.fork();
} else createServer((req, res) => {
	const path = join(dir, normalize(decodeURIComponent(new URL(req.url, "http://x").pathname)));
	let size;
	try {
		size = statSync(path).size;
	} catch {
		res.writeHead(404).end();
		return;
	}
	const headers = { "content-type": "video/mp4", "accept-ranges": "bytes", etag: `"${size}"` };
	const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range ?? "");
	if (range) {
		const start = range[1] === "" ? size - Number(range[2]) : Number(range[1]);
		const end = range[1] === "" || range[2] === "" ? size - 1 : Math.min(Number(range[2]), size - 1);
		if (start > end || start >= size) {
			res.writeHead(416, { "content-range": `bytes */${size}` }).end();
			return;
		}
		res.writeHead(206, { ...headers, "content-length": end - start + 1, "content-range": `bytes ${start}-${end}/${size}` });
		if (req.method === "HEAD") return res.end();
		createReadStream(path, { start, end }).pipe(res);
		return;
	}
	res.writeHead(200, { ...headers, "content-length": size });
	if (req.method === "HEAD") return res.end();
	createReadStream(path).pipe(res);
}).listen(Number(port), "127.0.0.1");
