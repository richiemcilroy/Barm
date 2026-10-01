const { z } = require("zod");
const User = z.object({ name: z.string().min(1), email: z.string().email(), age: z.number().int().nonnegative() });
const port = Number(process.env.PORT ?? 3000);
Bun.serve({
  port,
  async fetch(req) {
    try {
      const body = await req.json();
      const r = User.safeParse(body);
      if (r.success) return Response.json({ ok: true, name: r.data.name });
      return new Response("invalid", { status: 400 });
    } catch (e) {
      return new Response("bad json", { status: 400 });
    }
  },
});
