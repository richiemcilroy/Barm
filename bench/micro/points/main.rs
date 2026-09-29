// Small record value churn: every iteration builds a new {x, y} record.

#[derive(Clone, Copy)]
struct P {
    x: f64,
    y: f64,
}

fn add(a: P, b: P) -> P {
    P { x: a.x + b.x, y: a.y + b.y }
}

fn main() {
    let n = 600_000_000;
    let one = P { x: 1.0, y: 0.5 };
    let mut v = P { x: 0.0, y: 0.0 };
    for _ in 0..n {
        v = add(v, one);
    }
    println!("{:.1} {:.1}", v.x, v.y);
}
