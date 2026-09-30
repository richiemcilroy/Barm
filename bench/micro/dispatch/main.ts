// Virtual method calls: an array of shapes of three classes, `area()` called 50M times.

abstract class Shape {
  abstract area(): number;
}

class Circle extends Shape {
  readonly r: number;
  constructor(r: number) {
    super();
    this.r = r;
  }
  area(): number {
    return Math.PI * this.r * this.r;
  }
}

class Rect extends Shape {
  readonly w: number;
  readonly h: number;
  constructor(w: number, h: number) {
    super();
    this.w = w;
    this.h = h;
  }
  area(): number {
    return this.w * this.h;
  }
}

class Tri extends Shape {
  readonly b: number;
  readonly h: number;
  constructor(b: number, h: number) {
    super();
    this.b = b;
    this.h = h;
  }
  area(): number {
    return 0.5 * this.b * this.h;
  }
}

function main() {
  let state = 42;
  const shapes: Shape[] = [];
  for (let i = 0; i < 1000; i++) {
    state = (state * 16807) % 2147483647;
    const a = (state % 100) / 10 + 1;
    state = (state * 16807) % 2147483647;
    const b = (state % 100) / 10 + 1;
    const kind = state % 3;
    if (kind === 0) {
      shapes.push(new Circle(a));
    } else if (kind === 1) {
      shapes.push(new Rect(a, b));
    } else {
      shapes.push(new Tri(a, b));
    }
  }
  let total = 0.0;
  for (let round = 0; round < 50000; round++) {
    for (const s of shapes) {
      total += s.area();
    }
  }
  console.log(total.toFixed(3));
}

main();
