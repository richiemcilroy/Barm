// node:http — same routes as server.barm.ts. WORKERS>1 uses node:cluster.
import cluster from "node:cluster"
import http from "node:http"

const port = Number(process.env.PORT ?? 3000)
const workers = Number(process.env.WORKERS ?? 1)

function send(res, status, type, body) {
  res.writeHead(status, { "content-type": type, "content-length": Buffer.byteLength(body) })
  res.end(body)
}

if (workers > 1 && cluster.isPrimary) {
  for (let i = 0; i < workers; i++) cluster.fork()
} else {
  http
    .createServer((req, res) => {
      const q = req.url.indexOf("?")
      const path = q < 0 ? req.url : req.url.slice(0, q)
      if (path === "/") {
        send(res, 200, "text/plain;charset=utf-8", "Hello, World!")
      } else if (path === "/json") {
        send(res, 200, "application/json", JSON.stringify({ id: 1, name: "Ada Lovelace", email: "ada@example.com" }))
      } else if (path === "/echo" && req.method === "POST") {
        const chunks = []
        req.on("data", (c) => chunks.push(c))
        req.on("end", () => send(res, 200, "text/plain;charset=utf-8", Buffer.concat(chunks)))
      } else {
        send(res, 404, "text/plain;charset=utf-8", "not found")
      }
    })
    .listen(port)
}
