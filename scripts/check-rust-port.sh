#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

python3 scripts/verify-rust-policy.py
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --all-features -- -D warnings
cargo test --workspace --all-features
# Miri is intentionally run on pure first-party logic. OS/hardware adapters are
# exercised by normal tests because Miri cannot emulate those external devices.
cargo miri test -p ns-shared
