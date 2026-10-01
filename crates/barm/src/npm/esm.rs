//! ES modules → the bundle's CommonJS-style module functions.

/// Rewrites an ES module as a CommonJS function body; returns it with its static imports.
pub fn to_commonjs(_src: &str) -> Result<(String, Vec<String>), String> {
    Err("ES module syntax isn't supported yet (the package has no CommonJS build)".into())
}
