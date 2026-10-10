// JSON: stringify 200k records, then parse them back (typed and validated, with serde), 5 times.
use serde::{Deserialize, Serialize, Serializer};

#[derive(Serialize, Deserialize)]
struct Item {
    id: i64,
    name: String,
    // (written as JavaScript writes a number: 1, not 1.0)
    #[serde(serialize_with = "js_number")]
    price: f64,
    tags: Vec<String>,
    active: bool,
}

fn js_number<S: Serializer>(v: &f64, s: S) -> Result<S::Ok, S::Error> {
    if v.fract() == 0.0 && v.abs() < 9007199254740992.0 {
        s.serialize_i64(*v as i64)
    } else {
        s.serialize_f64(*v)
    }
}

fn main() {
    let items: Vec<Item> = (0..200_000)
        .map(|i| Item {
            id: i,
            name: format!("item {i}"),
            price: i as f64 * 0.25,
            tags: vec!["a".into(), "bb".into(), format!("t{}", i % 7)],
            active: i % 3 == 0,
        })
        .collect();
    let mut total = 0i64;
    let mut bytes = 0usize;
    for _ in 0..5 {
        let text = serde_json::to_string(&items).unwrap();
        bytes += text.len();
        let back: Vec<Item> = serde_json::from_str(&text).unwrap();
        total += back.len() as i64 + back[back.len() - 1].id;
    }
    println!("{total} {bytes}");
}
