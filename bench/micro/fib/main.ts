// Naive recursive Fibonacci: call overhead and integer arithmetic.

function fib(n: number): number {
  if (n < 2) return n
  return fib(n - 1) + fib(n - 2)
}

function main() {
  console.log(fib(42))
}

main()
