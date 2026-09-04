use std::path::{Path, PathBuf};

pub fn normalize_user_path(path: &Path) -> PathBuf {
    if path.is_absolute() { return path.to_path_buf(); }
    std::env::current_dir().map(|current| current.join(path)).unwrap_or_else(|_| path.to_path_buf())
}

pub fn display_error(context: &str, error: &dyn std::fmt::Display) -> String { format!("{context}: {error}") }
