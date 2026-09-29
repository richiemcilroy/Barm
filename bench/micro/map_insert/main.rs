// Insert N string keys into a string -> int map, then look every key up again.

use std::collections::HashMap;

fn main() {
    let n: i64 = 2_000_000;
    let mut m: HashMap<String, i64> = HashMap::new();
    for i in 0..n {
        m.insert(format!("k{i}"), i);
    }
    let mut sum: i64 = 0;
    for i in 0..n {
        sum += m[&format!("k{i}")];
    }
    println!("{} {}", m.len(), sum);
}
