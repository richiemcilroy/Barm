// JSON: stringify 200k records, then parse them back, 5 times.
type Item = { id: number, name: string, price: number, tags: string[], active: boolean }

function main() {
  const items: Item[] = []
  for (let i = 0; i < 200000; i++) {
    items.push({ id: i, name: `item ${i}`, price: i * 0.25, tags: ["a", "bb", `t${i % 7}`], active: i % 3 === 0 })
  }
  let total = 0
  let bytes = 0
  for (let round = 0; round < 5; round++) {
    const text = JSON.stringify(items)
    bytes += text.length
    const back: Item[] = JSON.parse(text)
    total += back.length + back[back.length - 1]!.id
  }
  console.log(total, bytes)
}
main();
