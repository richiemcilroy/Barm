// Awaited async calls: the cost of calling an async function and awaiting its result.

async fn step(total: i64, i: i64) -> i64 {
    total + i % 7
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let mut total = 0i64;
    for i in 0..30_000_000 {
        total = step(total, i).await;
    }
    println!("{total}");
}
