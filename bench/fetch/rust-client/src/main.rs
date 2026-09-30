// The Rust client: reqwest (hyper) with the same work as client.ts. MODE_RT=mt runs tokio's
// multi-thread runtime; the default is a current-thread runtime, one core like the others.
use serde::Deserialize;

#[derive(Deserialize)]
#[allow(dead_code)]
struct User {
    id: i64,
    name: String,
    email: String,
}

const PAYLOAD: &str = r#"{"id":1,"name":"Ada Lovelace","email":"ada@example.com","tags":["a","b","c"]}"#;

async fn one(client: &reqwest::Client, mode: &str, base: &str) -> usize {
    match mode {
        "json" => client.get(format!("{base}/json")).send().await.unwrap().json::<User>().await.unwrap().id as usize,
        "echo" => client.post(format!("{base}/echo")).header("content-type", "application/json").body(PAYLOAD).send().await.unwrap().text().await.unwrap().len(),
        "big" => client.get(format!("{base}/big")).send().await.unwrap().text().await.unwrap().len(),
        _ => client.get(format!("{base}/")).send().await.unwrap().text().await.unwrap().len(),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mode = args.get(1).cloned().unwrap_or_else(|| "hello".into());
    let base = args.get(2).cloned().unwrap_or_else(|| "http://127.0.0.1:3000".into());
    let total: usize = args.get(3).and_then(|v| v.parse().ok()).unwrap_or(10000);
    let conc: usize = args.get(4).and_then(|v| v.parse().ok()).unwrap_or(1);
    let rt = if std::env::var("MODE_RT").is_ok_and(|m| m == "mt") {
        tokio::runtime::Builder::new_multi_thread().enable_all().build().unwrap()
    } else {
        tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap()
    };
    rt.block_on(async move {
        let client = reqwest::Client::new();
        let start = std::time::Instant::now();
        let per = total / conc;
        let mut tasks = Vec::new();
        for _ in 0..conc {
            let (client, mode, base) = (client.clone(), mode.clone(), base.clone());
            tasks.push(tokio::spawn(async move {
                let mut sum = 0;
                for _ in 0..per {
                    sum += one(&client, &mode, &base).await;
                }
                sum
            }));
        }
        for t in tasks {
            t.await.unwrap();
        }
        println!("{mode} {} {:.1}", per * conc, start.elapsed().as_secs_f64() * 1000.0);
    });
}
