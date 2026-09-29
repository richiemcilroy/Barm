// Generate N pseudo-random floats with a Park-Miller LCG and sort them with a comparator.

function main() {
  const n = 3_000_000
  let state = 42
  const xs: number[] = []
  for (let i = 0; i < n; i++) {
    state = (state * 16807) % 2147483647
    xs.push(state / 2147483647)
  }
  xs.sort((a, b) => a - b)
  let checksum = 0.0
  for (let i = 0; i < n; i += 1000) {
    checksum += xs[i]!
  }
  console.log(`${xs[0]!.toFixed(9)} ${xs[n - 1]!.toFixed(9)} ${checksum.toFixed(6)}`)
}

main()
