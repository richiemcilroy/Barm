// Build strings with format!, join them with ",", split them back and measure the pieces.

fn main() {
    let n = 3_000_000;
    let mut parts: Vec<String> = Vec::new();
    for i in 0..n {
        parts.push(format!("item-{}-{}", i, i % 7));
    }
    let joined = parts.join(",");
    let back: Vec<&str> = joined.split(',').collect();
    let mut bytes = 0;
    let mut threes = 0;
    for s in &back {
        bytes += s.len();
        if s.ends_with("-3") {
            threes += 1;
        }
    }
    println!("{} {} {} {}", back.len(), joined.len(), bytes, threes);
}
