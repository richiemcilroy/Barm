// Bun.serve — same routes as server.barm. WORKERS>1 spawns processes with reusePort.
const port = Number(process.env.PORT ?? 3000)
const workers = Number(process.env.WORKERS ?? 1)

if (workers > 1 && !process.env.BUN_CHILD) {
  for (let i = 0; i < workers; i++) {
    Bun.spawn([process.execPath, import.meta.path], { env: { ...process.env, BUN_CHILD: "1" }, stdout: "inherit", stderr: "inherit" })
  }
} else {
  Bun.serve({
    port,
    reusePort: true,
    async fetch(req) {
      const path = new URL(req.url).pathname
      if (path === "/") return new Response("Hello, World!")
      if (path === "/json") return Response.json({ id: 1, name: "Ada Lovelace", email: "ada@example.com" })
      if (path === "/echo" && req.method === "POST") return new Response(await req.text())
      return new Response("not found", { status: 404 })
    },
  })
}
