const { z } = require("zod");
const User = z.object({ name: z.string(), age: z.number() });
const t0 = performance.now();
let ok = 0;
for (let i = 0; i < 1000000; i++) {
  const r = User.safeParse({ name: "Ada", age: i });
  if (r.success) ok++;
}
console.log(ok, Math.round(performance.now() - t0));
