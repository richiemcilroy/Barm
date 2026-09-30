# Barm language spec (draft 0)

Barm is TypeScript-shaped. **If code looks like TypeScript it behaves like TypeScript, or it is a compile error with a code and a fix.** This spec lists only what differs from, or is removed from, TypeScript; everything else means what it means in TS.

Status tags: **[M0]** checked today · **[Mn]** planned for milestone n.

## 1. Files, modules, entry point
- Source files end in `.barm`. One file = one module.
- `import { a, b } from "./other"` (relative, no extension) and `import * as fs from "std/fs"`. No default imports/exports, no `import x as y` renaming, no re-exports (`export { x } from`), no side-effect imports. **[M0]**
- `export` goes on declarations: `export function`, `export type`, `export interface`, `export const`, `export class` [M2].
- Top level may contain only declarations, `const` bindings, and `test(...)` calls. Mutable module state (`let` at top level) is an error. The program entry point is `function main()` (optionally `main(): int` for an exit code). **[M0]**

## 2. Lexical
- Comments `//`, `/* */`. Identifiers as in TS (ASCII + `_` + `$`).
- Integer literals: `42`, `1_000`, `0xff`, `0b1010`, `0o17`. Float literals contain `.` or an exponent: `1.0`, `.5`, `2e10`.
- Strings: `"..."` or `'...'` with JS escapes. Template literals `` `a ${expr} b` ``.
- Semicolons are optional. A line break ends a statement unless the next line starts with `.`, `?.`, or a binary operator, or the break is inside `()`/`[]`. A line starting with `(` or `[` never continues the previous line.

## 3. Types **[M0]**
| Type | Meaning |
|---|---|
| `int` | 64-bit signed integer (overflow traps) |
| `f64` (alias `number`), `f32` | IEEE floats |
| `i8 i16 i32 u8 u16 u32 u64` | sized integers |
| `bool` (alias `boolean`), `string` | UTF-8 immutable string |
| `undefined`, `void`, `never`, `unknown` | as TS |
| `T[]` / `Array<T>` | growable array (value) |
| `Map<K, V>`, `Set<T>` | hash map/set (value) |
| `{ a: T, b?: U }` | exact record (value) |
| `A \| B` | union; `"lit"` string-literal types |
| `(a: T) => U` | function type |

- Removed: `any`, `null` (use `undefined`), `object`, `symbol`, `bigint`, tuples [later], intersection `&`, conditional/mapped/indexed-access types, `keyof`, `typeof` in types, enums (use literal unions), namespaces, decorators.
- `number` is exactly `f64`; `boolean` is exactly `bool`.

### Numbers
- An integer literal is `int` unless context expects a float type. An unannotated `let x = 0` becomes `f64` if any later use in the function requires it (`x += 0.5`, `return x` from an `f64` function).
- `int` widens implicitly to `f64` (in arithmetic and assignment). `f64` → `int` is never implicit: use `Math.trunc/floor/ceil/round(x)`, which return `int` (trap on NaN/overflow).
- Float arithmetic is IEEE 754 with every operation rounded separately, exactly as in JavaScript: `a * b + c` is never fused into one multiply-add.
- `/` always produces `f64` (as in TS). `%`, `+ - *` on two ints produce `int`. Sized integers don't mix implicitly; convert with `u8(x)`, `i32(x)` etc. (trap if out of range).

### Records
- `type P = { x: f64, y: f64 }` declares an exact record. Two record types with the same fields are the same type.
- An object literal must provide every required field and no others. Optional fields (`b?: U`) may be omitted and have type `U | undefined`.
- Records, arrays, maps, sets, and strings are **values**: assignment copies (cheaply, copy-on-write). See §6.

### Unions and narrowing
- A **discriminated union** is a union of records sharing a string-literal field (e.g. `kind`). It compiles to a tagged union.
- Narrowing works on: `switch (x.kind)`, `if (x.kind === "a")`, `x === undefined` / `x !== undefined`, `typeof x === "string"` (for unions of primitives), and early exits (`if (x === undefined) return` narrows the rest of the block).
- Field paths of locals narrow too: `if (t.left !== undefined) use(t.left)`. Assigning to the path (or any prefix of it), or passing it as `inout`, forgets the narrowing. Inside a closure, narrowings of the enclosing function's `let` variables don't apply.
- A `switch` over a union discriminant or literal union must be exhaustive or have `default`.

### Interfaces **[M2]**
- `interface` is structural and implicit: any record or class with the members (and matching types) satisfies it. As a generic bound it is statically dispatched; as a value type (`function f(x: Named)`, `Shape[]`) it holds any satisfying value. A record behind an interface is a copy (records stay values); a class instance is shared.
- Methods in interfaces (`area(): f64`) are function-typed members: a class satisfies them with methods, a record with fields holding functions.

### Generics
- `function f<T>(x: T): T`, `function g<T extends I>(...)`, `type Box<T> = { value: T }`. Type arguments are inferred from arguments. Bodies are checked once, at definition: a `T` can only be used through its bound.

## 4. Declarations and statements **[M0]**
- `function`, `const`, `let`, `type`, `interface`, arrow functions `(x: T) => expr` / `=> { ... }`.
- Exported functions must annotate every parameter and the return type (unannotated return means `void`). Local and unexported functions may omit the return type when it can be inferred.
- `if/else`, `while`, `do/while`, `for (init; cond; step)`, `for (const x of xs)`, `switch`, `break`, `continue`, `return`, blocks.
- Conditions must be `bool`, or an optional of a non-primitive type (tests "is defined"). `if (n)` on a number or `if (s)` on a string is an error with the explicit rewrite (`n !== 0`, `s !== ""`).
- A function with a non-`void` return type must return on every path.
- Removed: `var`, `with`, labels, `for…in` (use `for (const k of map.keys())`), `delete`, `arguments`, `eval`, comma operator.

## 5. Expressions **[M0]**
- Operators and precedence as in TS. Equality is `===` / `!==`; `==` / `!=` are errors (fix: use `===`). Equality on records/arrays compares by value.
- `+` concatenates when either side is a `string` (numbers/bools format as in JS).
- `a?.b`, `a ?? b`, `c ? a : b`, `x as T` (checked cast from `unknown` or a union; traps on failure), `x!` (non-undefined assertion; traps).
- Indexing `xs[i]` requires an integer `i` and has type `T | undefined`; `xs[i]!` asserts presence (traps when out of bounds). Index assignment `xs[i] = v` traps when out of bounds.
- Strings have no `.length` (ambiguous under UTF-8). Use `s.byteLength` or `s.chars().length`.
- `console.log(...args)` prints (Node's formatting); `Math.*` as in JS (with the int-returning rounders above).
- `map.keys()`, `map.values()` and `set.values()` return arrays (not iterators).
- `parseInt(s)`, `parseFloat(s)` and `Number(s)` return `T | undefined` instead of `NaN`, so failed parses must be handled.

## 6. Values, mutation, `inout` **[M0]**
- `let` bindings can be reassigned; `const` bindings cannot. Both can be mutated in place (`xs.push(1)`, `p.x = 2`) — same as TS.
- Parameters are read-only views of the caller's value. Mutating one is an error. To mutate the caller's value declare `inout` and pass a `let` variable (or a field/element of one) with `&` — the callee may replace the whole value:
  ```ts
  function addOne(inout xs: int[]) { xs.push(1) }
  addOne(&items)
  ```
- Copy-then-mutate where the original is read again (`const b = a; b.push(1); use(a)`, or `const b = a; addOne(&a); use(b)`) is an error [M1], because TS would share and Barm copies.
- `class` instances are shared references (reference-counted): assignment shares the object, as in TS, and changes through one reference are seen through all. Their fields can be changed through any reference (`p.x = 1` works on a `const p` or a parameter). See §6a.

## 6a. Classes **[M2]**
```ts
abstract class Shape {
  constructor(readonly name: string) {}     // parameter property: a readonly field
  abstract area(): f64
  describe(): string { return `${this.name}: ${this.area()}` }
}
class Circle extends Shape {
  private hits = 0
  static readonly UNIT = 1
  constructor(private r: f64) { super("circle") }
  area(): f64 { return Math.PI * this.r * this.r }
  get diameter(): f64 { return 2 * this.r }
}
```
- Single inheritance (`extends`, `super(...)`, `super.method(...)`), `abstract` classes and methods, `implements` (checked structurally), `static` methods and `static readonly` fields (no mutable statics), getters (setters later), `public`/`protected`/`private` and `#private` members, `readonly` fields (set only by their initializer or the constructor), parameter properties. No generic methods yet (make the class generic, or use a top-level generic function).
- Every field needs a value: an initializer, an assignment at the top level of the constructor, or `?` (optional). In a constructor, `this` is only used to set fields until all are set. A derived class's constructor starts with `super(...)`.
- An override takes the same parameters (or wider types) and returns the same type (or a narrower one). Methods nobody overrides are called directly.
- `x instanceof C` narrows unions and base-class types; `x as C` is a checked downcast (traps if wrong). `===` on instances is identity. `console.log` prints `ClassName { field: value }`, and `${x}` uses a `toString(): string` method if the class has one.
- `C | undefined` fields and variables are nullable pointers (no tag word).
- **Memory.** Instances are reference-counted and freed as soon as the last reference goes. Reference counting can't free *cycles*, so the checker rejects class graphs where instances could form a cycle through strong fields, and offers three fixes: `weak parent: Node | undefined` (a back-reference that doesn't keep its target alive and reads as `undefined` once the target is freed); `readonly` references set in the constructor (objects then only point to older objects, so no cycle can form); or `cyclic class Node`, which also enrolls its instances (and those of classes on the same cycles, and subclasses) in a cycle collector. Only cyclic classes pay for it. Closures stored in fields that capture `this` are not traced: avoid storing such closures on the object they capture.

## 7. Errors **[M3]**
```ts
class NotFound extends Error {
  constructor(readonly key: string) { super(`no such key: ${key}`); this.name = "NotFound" }
}
function lookup(m: Map<string, int>, k: string): int throws NotFound {
  const v = m.get(k)
  if (v === undefined) throw new NotFound(k)
  return v
}
function total(m: Map<string, int>): int {   // throws NotFound (inferred)
  return try lookup(m, "a") + try lookup(m, "b")
}
try {
  console.log(total(m))
} catch (e) {                                  // e: NotFound
  console.log(e.message)
} finally {
  cleanup()
}
```
- Only `Error` objects (the built-in `Error`, `TypeError`, `RangeError`, `SyntaxError`, or classes extending them) are thrown. `e.message`, `e.name`, and `String(e)` / `${e}` (`"Name: message"`) work as in JavaScript.
- A call that can throw is either inside a `try { }` block, or marked `try f(x)`: the error then passes on to the caller. Unmarked calls are errors with the fix. `throws E` declares what a function can throw; unexported functions and methods infer it, exported ones must declare it.
- `catch (e)` gives `e` the most specific class covering everything the block can throw (else `Error`); narrow with `instanceof`. `finally` runs on every path out of the block (no `return`/`break` inside a `try` that has `finally` yet).
- Closures can't pass errors on yet: catch inside them. An error that escapes `main` prints `uncaught Name: message` to stderr and exits with status 1; in a test it fails the test.
- Errors are return values underneath (no unwinding): a call that can throw costs one branch.
- Bugs (index out of range with `!`, integer overflow, failed `as`) are traps, not errors: they can't be caught.
- Effects (`uses fs | net`) are planned.

## 7a. Standard library **[M3]**
Node's names, so existing habits work:
- `import { readFileSync, writeFileSync, appendFileSync, existsSync, readdirSync, mkdirSync, rmSync, unlinkSync } from "node:fs"` (also `"fs"`, `"std/fs"`): the synchronous API; failures throw `Error` with Node's messages (`ENOENT: no such file or directory, open 'x'`). `readdirSync` returns sorted names.
- `import * as path from "node:path"`: `join`, `resolve`, `normalize`, `basename`, `dirname`, `extname`, `isAbsolute`, `sep` (POSIX).
- `process.argv` (`[binary, binary, ...args]`, so `process.argv.slice(2)` are the arguments), `process.env.NAME` (`string | undefined`), `process.exit(code?)`, `process.cwd()`, `process.platform`, `process.stdout.write(s)`, `process.stderr.write(s)`; `console.error` / `console.warn` write to stderr.
- `Date.now()` and `performance.now()` (milliseconds).
- `JSON.stringify(value, undefined, indent?)` as in JavaScript (maps with string keys are written as objects, where JavaScript writes `{}`). `JSON.parse(text)` takes its type from context — `const c: Config = try JSON.parse(text)` or `try JSON.parse(text) as Config` — and **checks** the input against it (exact integers, string literals, discriminants, required fields; extra keys ignored, `null` and missing keys read as `undefined`), throwing `SyntaxError` otherwise.
- Rest parameters: `function f(first: T, ...rest: U[])` (not in methods yet).
- `import { serve, Request, Response, Headers, json } from "std/http"`: a Bun-style HTTP/1.1 server.
  ```ts
  function route(req: Request): Response {
    if (req.pathname === "/") return new Response("hello")
    if (req.method === "POST") return new Response(req.text(), { status: 201 })
    return json(JSON.stringify({ error: "not found" }), 404)
  }
  function main(): void throws Error {
    try serve({ port: 3000, workers: 8, fetch: route })   // blocks; throws if the port can't be bound
  }
  ```
  `Request`: `method`, `url` (`http://host/path?q`), `pathname`, `search`, `headers` (a `Headers`), `header(name)`, `text()`. `Response(body?, { status?, headers? })`; `json(body, status?)` sets `content-type: application/json`. `Headers`: `get`/`has`/`set`/`append`/`delete`, case-insensitive; CR/LF in names and values are dropped. The server handles keep-alive, pipelining, `Content-Length` and chunked request bodies, `Expect: 100-continue` and `HEAD`, and adds `content-length`, `date`, and `content-type: text/plain;charset=utf-8` when none is set; malformed or oversized requests get `400`/`413`/`431` and are closed. `workers` forks that many processes that share the listening socket, and connections are spread evenly between them. The handler can't throw (catch inside it); it runs to completion for each request (no `async` yet).

## 8. Tests **[M0]**
```ts
test("adds", () => {
  expect(add(1, 2)).toBe(3)
})
```
`test` calls are top-level only. Matchers: `toBe`, `toEqual`, `toBeCloseTo`, `toBeUndefined`, `toBeDefined`.

## 9. Diagnostics
Every diagnostic has a stable code, a location, the observed problem, **the valid alternatives**, and — where possible — a fix tagged `safe` (apply automatically), `maybe` (review), or `placeholder` (needs input). `barm explain <CODE>` prints details.

Code prefixes: `L` lexing · `P` parsing · `N` names/imports · `T` types · `F` control flow · `V` values/mutation · `X` removed TypeScript feature.
