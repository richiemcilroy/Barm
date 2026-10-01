const { z } = require("zod");
const User = z.object({ name: z.string(), age: z.number() });
console.log(User.safeParse({ name: "Ada", age: 36 }).success);
