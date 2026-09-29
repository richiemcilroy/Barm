// The Benchmarks Game binary-trees: allocate, walk and free many perfect binary trees.

struct Tree {
    left: Option<Box<Tree>>,
    right: Option<Box<Tree>>,
}

fn bottom_up(depth: u32) -> Box<Tree> {
    if depth == 0 {
        return Box::new(Tree { left: None, right: None });
    }
    Box::new(Tree {
        left: Some(bottom_up(depth - 1)),
        right: Some(bottom_up(depth - 1)),
    })
}

fn check(t: &Tree) -> i64 {
    match (&t.left, &t.right) {
        (Some(left), Some(right)) => 1 + check(left) + check(right),
        _ => 1,
    }
}

fn main() {
    let n = 18;
    let min_depth = 4;
    let max_depth = std::cmp::max(min_depth + 2, n);

    let stretch_depth = max_depth + 1;
    println!("stretch tree of depth {}\t check: {}", stretch_depth, check(&bottom_up(stretch_depth)));

    let long_lived = bottom_up(max_depth);

    for depth in (min_depth..=max_depth).step_by(2) {
        let iterations: i64 = 1 << (max_depth - depth + min_depth);
        let mut sum = 0;
        for _ in 0..iterations {
            sum += check(&bottom_up(depth));
        }
        println!("{}\t trees of depth {}\t check: {}", iterations, depth, sum);
    }

    println!("long lived tree of depth {}\t check: {}", max_depth, check(&long_lived));
}
