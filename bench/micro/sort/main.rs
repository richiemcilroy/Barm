// Generate N pseudo-random floats with a Park-Miller LCG and sort them with a comparator.

fn main() {
    let n: usize = 3_000_000;
    let mut state: i64 = 42;
    let mut xs: Vec<f64> = Vec::new();
    for _ in 0..n {
        state = (state * 16807) % 2147483647;
        xs.push(state as f64 / 2147483647.0);
    }
    xs.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let mut checksum = 0.0;
    for i in (0..n).step_by(1000) {
        checksum += xs[i];
    }
    println!("{:.9} {:.9} {:.6}", xs[0], xs[n - 1], checksum);
}
