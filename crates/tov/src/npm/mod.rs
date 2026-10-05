//! npm packages: Node's module resolution, and a bundler that turns a program's packages (and
//! the Node built-ins they use) into one script for the embedded JavaScript engine.

pub mod bundle;
pub mod esm;
pub mod json;
pub mod lex;
pub mod node_shims;
pub mod parse;
pub mod resolve;

pub use bundle::{bundle, Bundle};
