pub mod ast;

/// Real async (`BARM_ASYNC=1`) while it's being built: `await` suspends and `Promise<T>` is a
/// type of its own. Without it, async is synchronous: `await e` is `e` and `Promise<T>` is `T`.
pub fn async_enabled() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("BARM_ASYNC").is_some_and(|v| v != "0"))
}
pub mod build;
pub mod check;
pub mod codegen;
pub mod codes;
pub mod diag;
pub mod driver;
pub mod hash;
pub mod intern;
pub mod lexer;
pub mod parser;
pub mod source;
pub mod types;
