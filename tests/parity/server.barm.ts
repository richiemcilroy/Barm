// The Barm port of server.ts: the same file with `try` on calls that can throw and `byteLength`
// for `length` on strings. Diff them to see every edit.
//

type Todo = { id: number; title: string; done: boolean }

const todos: Todo[] = [
  { id: 1, title: "write the parser", done: true },
  { id: 2, title: "ship the server", done: false },
]
let requests = 0

function findTodo(id: string): Todo | undefined {
  return todos.find((t) => t.id === Number(id))
}

const server = Bun.serve({
  port: Number(process.env.PORT ?? 3000),
  routes: {
    "/": new Response("Welcome"),
    "/health": () => Response.json({ ok: true, requests }),
    "/todos": {
      GET: () => Response.json(todos),
      POST: async (req) => {
        const input = try (await req.json()) as { title: string }
        const todo: Todo = { id: todos.length + 1, title: input.title, done: false }
        todos.push(todo)
        return Response.json(todo, { status: 201 })
      },
    },
    "/todos/:id": (req) => {
      const todo = findTodo(req.params.id)
      if (todo === undefined) {
        return Response.json({ error: "not found" }, { status: 404 })
      }
      return Response.json(todo)
    },
    "/users/:user/posts/:post": (req) => Response.json(req.params),
    "/static/*": (req) => new Response(`static ${(try new URL(req.url)).pathname}`),
    "/static/:file": (req) => new Response(`file ${req.params.file}`),
    "/only-get": { GET: () => new Response("got it") },
    "/boom": () => {
      throw new Error("kaboom")
    },
    "/empty": () => new Response(undefined, { status: 204 }),
  },
  async fetch(req) {
    requests++
    const url = try new URL(req.url)
    if (url.pathname === "/echo" && req.method === "POST") {
      const text = await req.text()
      return new Response(text, { headers: { "Content-Type": "text/plain", "X-Length": String(text.byteLength) } })
    }
    if (url.pathname === "/search") {
      const q = url.searchParams.get("q") ?? ""
      return Response.json({ q, tags: url.searchParams.getAll("tag"), page: Number(url.searchParams.get("page") ?? "1") })
    }
    if (url.pathname === "/redirect") {
      return Response.redirect("/", 302)
    }
    if (url.pathname === "/headers") {
      const agent = req.headers.get("user-agent") ?? "unknown"
      const custom = req.headers.get("X-Custom") ?? "none"
      return new Response(`agent=${agent} custom=${custom} method=${req.method}`, { status: 200, headers: { "Cache-Control": "no-store" } })
    }
    return new Response(`Not Found: ${url.pathname}`, { status: 404 })
  },
})

console.log(`Listening on ${server.url}`)
