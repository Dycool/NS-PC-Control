#!/usr/bin/env python3
"""Fail closed unless Raw Gadget is the sole Rust unsafe-code boundary."""
from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[1]
SAFE_CRATES = (
    ROOT / "rust" / "shared",
    ROOT / "rust" / "server",
    ROOT / "rust" / "client",
)
BOUNDARY = ROOT / "rust" / "raw-gadget"
BOUNDARY_NAME = "ns-raw-gadget"
errors: list[str] = []


def rel(path: pathlib.Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT.resolve()))
    except ValueError:
        return str(path)


def manifest_data(path: pathlib.Path) -> dict:
    return tomllib.loads(path.read_text(encoding="utf-8"))


# First-party application crates remain compiler-forbidden from unsafe.
for crate in SAFE_CRATES:
    manifest = crate / "Cargo.toml"
    text = manifest.read_text(encoding="utf-8")
    required = (
        '[lints.rust]\nunsafe_code = "forbid"',
        'transmute_ptr_to_ptr = "forbid"',
        'undocumented_unsafe_blocks = "forbid"',
    )
    for needle in required:
        if needle not in text:
            errors.append(f"{rel(manifest)}: missing required safety lint: {needle!r}")
    if (crate / "build.rs").exists():
        errors.append(f"{rel(crate / 'build.rs')}: build scripts are forbidden in first-party crates")
    roots = [path for path in (crate / "src" / "lib.rs", crate / "src" / "main.rs") if path.exists()]
    if not roots:
        errors.append(f"{rel(crate)}: crate has no lib.rs or main.rs")
    for root in roots:
        if "#![forbid(unsafe_code)]" not in root.read_text(encoding="utf-8"):
            errors.append(f"{rel(root)}: missing #![forbid(unsafe_code)]")

# Exactly one unsafe boundary exists and it cannot hide transitive dependencies.
boundary_manifest = BOUNDARY / "Cargo.toml"
if not boundary_manifest.exists():
    errors.append("rust/raw-gadget/Cargo.toml: required exclusive unsafe boundary is missing")
else:
    boundary = manifest_data(boundary_manifest)
    if boundary.get("package", {}).get("name") != BOUNDARY_NAME:
        errors.append(f"{rel(boundary_manifest)}: boundary package must be named {BOUNDARY_NAME!r}")
    for section in ("dependencies", "dev-dependencies", "build-dependencies"):
        if boundary.get(section):
            errors.append(f"{rel(boundary_manifest)}: {section} must remain empty")
    if (BOUNDARY / "build.rs").exists():
        errors.append(f"{rel(BOUNDARY / 'build.rs')}: unsafe boundary build scripts are forbidden")
    boundary_text = boundary_manifest.read_text(encoding="utf-8")
    for needle in ('unsafe_op_in_unsafe_fn = "deny"', 'undocumented_unsafe_blocks = "deny"'):
        if needle not in boundary_text:
            errors.append(f"{rel(boundary_manifest)}: missing boundary hardening lint {needle!r}")

# A global -Funsafe-code would also disable the one explicitly approved boundary.
# Compiler-level forbids live in each safe crate manifest/root instead.
cargo_config = (ROOT / ".cargo" / "config.toml").read_text(encoding="utf-8")
if "-Funsafe-code" in cargo_config or "-Aunsafe-code" in cargo_config:
    errors.append(".cargo/config.toml: global unsafe-code rustflags are forbidden; safe crates enforce forbid locally")

attribute_override = re.compile(r"#\s*!?\s*\[\s*(?:allow|warn)\s*\(")
raw_pointer = re.compile(r"\*(?:const|mut)\b")
unsafe_token = re.compile(r"\bunsafe\b")
extern_c = re.compile(r'\bextern\s+"C"')
forbidden_tokens = (
    "std::mem::transmute", "core::mem::transmute", "std::ptr::", "core::ptr::",
    "MaybeUninit", "NonNull", "asm!", "global_asm!", "#[no_mangle]",
)

# First-party code may not contain safety escape hatches at all.
for crate in SAFE_CRATES:
    for path in sorted(crate.rglob("*.rs")):
        source = path.read_text(encoding="utf-8")
        code_lines = "\n".join(line.split("//", 1)[0] for line in source.splitlines())
        if attribute_override.search(code_lines):
            errors.append(f"{rel(path)}: allow/warn lint overrides are forbidden")
        if raw_pointer.search(code_lines):
            errors.append(f"{rel(path)}: raw pointer types are forbidden in first-party Rust")
        stripped_forbid = code_lines.replace("#![forbid(unsafe_code)]", "")
        if unsafe_token.search(stripped_forbid):
            errors.append(f"{rel(path)}: unsafe token is forbidden outside {BOUNDARY_NAME}")
        if extern_c.search(code_lines):
            errors.append(f"{rel(path)}: C FFI is forbidden outside {BOUNDARY_NAME}")
        for forbidden in forbidden_tokens:
            if forbidden in code_lines:
                errors.append(f"{rel(path)}: forbidden escape hatch {forbidden}")

# The boundary itself must document every unsafe block and may not weaken lints.
for path in sorted(BOUNDARY.rglob("*.rs")) if BOUNDARY.exists() else ():
    source = path.read_text(encoding="utf-8")
    code_lines = "\n".join(line.split("//", 1)[0] for line in source.splitlines())
    if attribute_override.search(code_lines):
        errors.append(f"{rel(path)}: allow/warn lint overrides are forbidden even in the boundary")

# Resolve the real Cargo graph. Every non-workspace dependency is scanned for
# unsafe source; only ns-raw-gadget may contain it. This catches transitive crates.
try:
    metadata = json.loads(subprocess.check_output(
        ["cargo", "metadata", "--format-version", "1"],
        cwd=ROOT,
        text=True,
        stderr=subprocess.STDOUT,
    ))
except (subprocess.CalledProcessError, FileNotFoundError, json.JSONDecodeError) as exc:
    errors.append(f"cargo metadata failed while enforcing dependency safety: {exc}")
    metadata = {"packages": []}

workspace_ids = set(metadata.get("workspace_members", []))
for package in metadata.get("packages", []):
    package_id = package.get("id", "")
    name = package.get("name", "")
    manifest_path = pathlib.Path(package.get("manifest_path", ""))
    package_root = manifest_path.parent
    if name == BOUNDARY_NAME:
        if package_id not in workspace_ids:
            errors.append(f"{BOUNDARY_NAME}: unsafe boundary must be the pinned workspace package")
        continue
    # Workspace safe crates were exhaustively checked above. External/path
    # dependencies are additionally scanned here so future additions cannot
    # silently introduce another unsafe island.
    if package_id in workspace_ids:
        continue
    for path in sorted(package_root.rglob("*.rs")):
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        code_lines = "\n".join(line.split("//", 1)[0] for line in source.splitlines())
        if unsafe_token.search(code_lines) or extern_c.search(code_lines) or raw_pointer.search(code_lines):
            errors.append(f"dependency {name!r} ({manifest_path}) contains unsafe/FFI/raw-pointer Rust; only {BOUNDARY_NAME!r} is permitted")
            break

if errors:
    print("Rust safety policy violations:", file=sys.stderr)
    for error in errors:
        print(f" - {error}", file=sys.stderr)
    raise SystemExit(1)

safe_count = sum(1 for crate in SAFE_CRATES for _ in crate.rglob("*.rs"))
boundary_count = sum(1 for _ in BOUNDARY.rglob("*.rs")) if BOUNDARY.exists() else 0
print(
    f"Rust safety policy OK ({safe_count} safe first-party files; "
    f"{boundary_count} file(s) in exclusive {BOUNDARY_NAME} unsafe boundary)"
)
