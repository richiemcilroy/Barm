// Awaited async calls: the cost of calling an async function and awaiting its result.

async function step(total: number, i: number): Promise<number> {
  return total + (i % 7)
}

async function main() {
  let total = 0
  for (let i = 0; i < 30_000_000; i++) {
    total = await step(total, i)
  }
  console.log(total)
}

main()
