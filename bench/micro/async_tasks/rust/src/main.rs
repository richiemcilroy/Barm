// Concurrent tasks: 1,000 async functions each yielding to the event loop 10,000 times.

async fn worker(id: i64, rounds: i64) -> i64 {
    let mut sum = 0;
    for i in 0..rounds {
        tokio::task::yield_now().await;
        sum += (id + i) % 3;
    }
    sum
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let tasks: Vec<_> = (0..1000).map(|id| tokio::spawn(worker(id, 10_000))).collect();
    let mut total = 0;
    for t in tasks {
        total += t.await.unwrap();
    }
    println!("{total}");
}
