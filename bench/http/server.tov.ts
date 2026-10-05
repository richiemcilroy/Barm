// The Tov port of server.bun.ts (same routes, same code; `try` marks the calls that can
// throw). WORKERS>1 uses Tov's `workers` option instead of spawning processes.
const port = Number(process.env.PORT ?? 3000)
const workers = Math.trunc(Number(process.env.WORKERS ?? 1) ?? 1)

Bun.serve({
  port,
  workers,
  async fetch(req) {
    const path = (try new URL(req.url)).pathname
    if (path === "/") return new Response("Hello, World!")
    if (path === "/json") return Response.json({ id: 1, name: "Ada Lovelace", email: "ada@example.com" })
    if (path === "/echo" && req.method === "POST") return new Response(await req.text())
    return new Response("not found", { status: 404 })
  },
})
