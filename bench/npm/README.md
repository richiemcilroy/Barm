# npm packages from Tov

A Tov program using an npm package (zod), against the same program in Bun. Run `bun install`
here first, then `bench/npm/run.py`.

| | what it measures |
|---|---|
| `startup` | start, load zod, validate one object, exit (wall time, median of 20) |
| `loop` | a million `safeParse` calls, each with an object built for it: every call crosses from Tov into JavaScript, and each does little work (the bridge's worst case) |
| `service` | an HTTP server validating each request's JSON body with zod (Tov's server against `Bun.serve`), driven by `bench/http`'s load generator (64 connections, 4 threads, 5 s) |

How Tov runs npm code: the packages are bundled when the program is built (resolved like
Node.js; ES modules, TypeScript and JSX converted), embedded in the binary, and run on the
system's JavaScriptCore (the engine Bun uses), each module compiled when it's first required.
Calls go through runtime/js.c: numbers, booleans and `undefined` cross without engine calls, and
a JavaScript expression in Tov code (`User.safeParse({ name, age })`) compiles to one engine call.

## Results

Apple M4 Max, macOS, Bun 1.3. These runs were on a busy machine (load average 3 to 5), so read
them as indicative. macOS caps loopback HTTP at about 180k round trips/s, which hides
differences in req/s; cpu/req is the measure that shows them.

| | Tov | Bun |
|---|---:|---:|
| startup | 9.6 ms | 9.9 ms |
| startup memory (RSS) | 15.1 MB | 16.5 MB |
| 1M zod calls | 195 ms | 97 ms |
| service req/s | 162k | 130k |
| service cpu/req | **6.3 µs** | 8.0 µs |
| service p99 | 735 µs | 983 µs |
| service memory (RSS) | 43 MB | 48 MB |
| binary size | **2.4 MB** | 64 MB (`bun build --compile`) |
| rebuild after an edit | **83 ms** | 483 ms (`bun build --compile`) |

Where Tov is behind:
- The call-heavy loop. Each call into JavaScript costs about 40 ns of engine entry and exit,
  and Bun has no such boundary. In a service, a request crosses once or twice, and Tov's
  server more than makes up for it.
- Startup is at parity. JavaScriptCore's JIT setup (about 1.6 ms at launch) and the system
  framework's load are the floor. JavaScriptCore's bytecode cache isn't open to programs
  outside Apple's sandbox ("data vault" directories), so modules are compiled on each run,
  lazily.
