// Binary-trees with class instances (reference types): allocation, method calls and freeing.

class Tree {
  readonly left: Tree | undefined;
  readonly right: Tree | undefined;
  constructor(left: Tree | undefined, right: Tree | undefined) {
    this.left = left;
    this.right = right;
  }

  check(): number {
    const left = this.left;
    const right = this.right;
    if (left === undefined || right === undefined) return 1;
    return 1 + left.check() + right.check();
  }
}

function bottomUp(depth: number): Tree {
  if (depth === 0) return new Tree(undefined, undefined);
  return new Tree(bottomUp(depth - 1), bottomUp(depth - 1));
}

function main() {
  const n = 18;
  const minDepth = 4;
  const maxDepth = Math.max(minDepth + 2, n);

  const stretchDepth = maxDepth + 1;
  console.log(`stretch tree of depth ${stretchDepth}\t check: ${bottomUp(stretchDepth).check()}`);

  const longLived = bottomUp(maxDepth);

  for (let depth = minDepth; depth <= maxDepth; depth += 2) {
    const iterations = 1 << (maxDepth - depth + minDepth);
    let sum = 0;
    for (let i = 0; i < iterations; i++) {
      sum += bottomUp(depth).check();
    }
    console.log(`${iterations}\t trees of depth ${depth}\t check: ${sum}`);
  }

  console.log(`long lived tree of depth ${maxDepth}\t check: ${longLived.check()}`);
}

main();
