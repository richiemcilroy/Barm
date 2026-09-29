// Build strings with template literals, join them, split them back and measure the pieces.

function main() {
  const n = 3_000_000
  const parts: string[] = []
  for (let i = 0; i < n; i++) {
    parts.push(`item-${i}-${i % 7}`)
  }
  const joined = parts.join(",")
  const back = joined.split(",")
  let bytes = 0
  let threes = 0
  for (const s of back) {
    bytes += s.length
    if (s.endsWith("-3")) threes++
  }
  console.log(`${back.length} ${joined.length} ${bytes} ${threes}`)
}

main()
