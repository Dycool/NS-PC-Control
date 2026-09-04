#!/usr/bin/env python3
"""Fail closed if Rust safety invariants are weakened."""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CRATES = (ROOT / "rust" / "shared", ROOT / "rust" / "server", ROOT / "rust" / "client")
errors: list[str] = []

for crate in CRATES:
    manifest = crate / "Cargo.toml"
    text = manifest.read_text(encoding="utf-8")
    required = (
        '[lints.rust]\nunsafe_code = "forbid"',
        'transmute_ptr_to_ptr = "forbid"',
        'undocumented_unsafe_blocks = "forbid"',
    )
    for needle in required:
        if needle not in text:
            errors.append(f"{manifest.relative_to(ROOT)}: missing required safety lint: {needle!r}")
    if (crate / "build.rs").exists():
        errors.append(f"{(crate / 'build.rs').relative_to(ROOT)}: build scripts are forbidden in first-party crates")

    roots = [crate / "src" / "lib.rs", crate / "src" / "main.rs"]
    roots = [path for path in roots if path.exists()]
    if not roots:
        errors.append(f"{crate.relative_to(ROOT)}: crate has no lib.rs or main.rs")
    for root in roots:
        source = root.read_text(encoding="utf-8")
        if "#![forbid(unsafe_code)]" not in source:
            errors.append(f"{root.relative_to(ROOT)}: missing #![forbid(unsafe_code)]")

rust_files = sorted((ROOT / "rust").rglob("*.rs"))
attribute_override = re.compile(r"#\s*!?\s*\[\s*(?:allow|warn)\s*\(")
raw_pointer = re.compile(r"\*(?:const|mut)\b")
for path in rust_files:
    source = path.read_text(encoding="utf-8")
    if attribute_override.search(source):
        errors.append(f"{path.relative_to(ROOT)}: allow/warn lint overrides are forbidden")
    if raw_pointer.search(source):
        errors.append(f"{path.relative_to(ROOT)}: raw pointer types are forbidden in first-party Rust")
    for forbidden in ("std::mem::transmute", "core::mem::transmute", "std::ptr::", "core::ptr::"):
        if forbidden in source:
            errors.append(f"{path.relative_to(ROOT)}: forbidden escape hatch {forbidden}")

if errors:
    print("Rust safety policy violations:", file=sys.stderr)
    for error in errors:
        print(f" - {error}", file=sys.stderr)
    raise SystemExit(1)

print(f"Rust safety policy OK ({len(rust_files)} first-party Rust files checked)")
