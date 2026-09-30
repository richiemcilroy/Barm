// Concurrent tasks: 1,000 async functions each yielding to the event loop 10,000 times.

async function worker(id: number, rounds: number): Promise<number> {
  let sum = 0
  for (let i = 0; i < rounds; i++) {
    await Promise.resolve(0)
    sum += (id + i) % 3
  }
  return sum
}

async function main() {
  const tasks: Promise<number>[] = []
  for (let id = 0; id < 1000; id++) {
    tasks.push(worker(id, 10_000))
  }
  const sums = await Promise.all(tasks)
  let total = 0
  for (const s of sums) {
    total += s
  }
  console.log(total)
}

main()
