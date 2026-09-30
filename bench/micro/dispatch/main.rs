// Virtual method calls: an array of shapes of three classes, `area()` called 50M times.

trait Shape {
    fn area(&self) -> f64;
}

struct Circle {
    r: f64,
}
struct Rect {
    w: f64,
    h: f64,
}
struct Tri {
    b: f64,
    h: f64,
}

impl Shape for Circle {
    fn area(&self) -> f64 {
        std::f64::consts::PI * self.r * self.r
    }
}
impl Shape for Rect {
    fn area(&self) -> f64 {
        self.w * self.h
    }
}
impl Shape for Tri {
    fn area(&self) -> f64 {
        0.5 * self.b * self.h
    }
}

fn main() {
    let mut state: i64 = 42;
    let mut shapes: Vec<Box<dyn Shape>> = Vec::new();
    for _ in 0..1000 {
        state = (state * 16807) % 2147483647;
        let a = (state % 100) as f64 / 10.0 + 1.0;
        state = (state * 16807) % 2147483647;
        let b = (state % 100) as f64 / 10.0 + 1.0;
        let kind = state % 3;
        if kind == 0 {
            shapes.push(Box::new(Circle { r: a }));
        } else if kind == 1 {
            shapes.push(Box::new(Rect { w: a, h: b }));
        } else {
            shapes.push(Box::new(Tri { b: a, h: b }));
        }
    }
    let mut total = 0.0;
    for _ in 0..50000 {
        for s in &shapes {
            total += s.area();
        }
    }
    println!("{:.3}", total);
}
