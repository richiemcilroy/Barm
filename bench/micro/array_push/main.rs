// Push N floats into a growable array, then sum it with for-in and with an indexed loop.

fn main() {
    let n = 30_000_000;
    let mut xs: Vec<f64> = Vec::new();
    for i in 0..n {
        xs.push(i as f64 * 0.5);
    }
    let mut sum_of = 0.0;
    for x in &xs {
        sum_of += x;
    }
    let mut sum_idx = 0.0;
    for i in 0..xs.len() {
        sum_idx += xs[i];
    }
    println!("{} {:.1} {:.1}", xs.len(), sum_of, sum_idx);
}
