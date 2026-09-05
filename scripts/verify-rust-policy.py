#!/usr/bin/env python3
"""Fail closed around a minimal, audited set of Rust unsafe/FFI boundaries."""
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
ALLOWLIST_PATH = ROOT / "scripts" / "rust-unsafe-allowlist.toml"
errors: list[str] = []


def rel(path: pathlib.Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT.resolve()))
    except ValueError:
        return str(path)


def manifest_data(path: pathlib.Path) -> dict:
    return tomllib.loads(path.read_text(encoding="utf-8"))


def load_unsafe_allowlist() -> dict[str, str]:
    if not ALLOWLIST_PATH.exists():
        errors.append(f"{rel(ALLOWLIST_PATH)}: required audited unsafe dependency allowlist is missing")
        return {}
    try:
        data = manifest_data(ALLOWLIST_PATH)
    except (OSError, tomllib.TOMLDecodeError) as exc:
        errors.append(f"{rel(ALLOWLIST_PATH)}: cannot parse allowlist: {exc}")
        return {}
    packages = data.get("packages", {})
    if not isinstance(packages, dict):
        errors.append(f"{rel(ALLOWLIST_PATH)}: [packages] must be a TOML table")
        return {}
    output: dict[str, str] = {}
    for package, reason in packages.items():
        if not isinstance(package, str) or "@" not in package:
            errors.append(
                f"{rel(ALLOWLIST_PATH)}: unsafe exception keys must be exact 'package@version' strings"
            )
            continue
        if package.startswith(f"{BOUNDARY_NAME}@"):
            errors.append(
                f"{rel(ALLOWLIST_PATH)}: {BOUNDARY_NAME!r} is the dedicated first-party boundary and must not be allowlisted here"
            )
            continue
        if not isinstance(reason, str) or not reason.strip():
            errors.append(f"{rel(ALLOWLIST_PATH)}: {package!r} needs a non-empty parity justification")
            continue
        output[package] = reason.strip()
    return output


unsafe_allowlist = load_unsafe_allowlist()

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
        errors.append(f"{rel(crate / 'build.rs')}: build scripts are forbidden in first-party application crates")
    roots = [path for path in (crate / "src" / "lib.rs", crate / "src" / "main.rs") if path.exists()]
    if not roots:
        errors.append(f"{rel(crate)}: crate has no lib.rs or main.rs")
    for root in roots:
        if "#![forbid(unsafe_code)]" not in root.read_text(encoding="utf-8"):
            errors.append(f"{rel(root)}: missing #![forbid(unsafe_code)]")

# Raw Gadget remains the sole first-party unsafe boundary and cannot hide
# transitive dependencies. Additional exceptions may only be external crates
# pinned by exact package name + version in rust-unsafe-allowlist.toml.
boundary_manifest = BOUNDARY / "Cargo.toml"
if not boundary_manifest.exists():
    errors.append("rust/raw-gadget/Cargo.toml: required first-party unsafe boundary is missing")
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

# A global -Funsafe-code would also disable the explicitly approved boundaries.
# Compiler-level forbids live in every first-party safe crate instead.
cargo_config = (ROOT / ".cargo" / "config.toml").read_text(encoding="utf-8")
if "-Funsafe-code" in cargo_config or "-Aunsafe-code" in cargo_config:
    errors.append(
        ".cargo/config.toml: global unsafe-code rustflags are forbidden; safe crates enforce forbid locally"
    )

attribute_override = re.compile(r"#\s*!?\s*\[\s*(?:allow|warn)\s*\(")
raw_pointer = re.compile(r"\*(?:const|mut)\b")
unsafe_token = re.compile(r"\bunsafe\b")
extern_c = re.compile(r'\bextern\s+"C"')
forbidden_tokens = (
    "std::mem::transmute",
    "core::mem::transmute",
    "std::ptr::",
    "core::ptr::",
    "MaybeUninit",
    "NonNull",
    "asm!",
    "global_asm!",
    "#[no_mangle]",
)

# First-party application code may not contain safety escape hatches at all.
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

# The first-party Raw Gadget boundary itself must document every unsafe block
# and may never weaken lints.
for path in sorted(BOUNDARY.rglob("*.rs")) if BOUNDARY.exists() else ():
    source = path.read_text(encoding="utf-8")
    code_lines = "\n".join(line.split("//", 1)[0] for line in source.splitlines())
    if attribute_override.search(code_lines):
        errors.append(f"{rel(path)}: allow/warn lint overrides are forbidden even in the boundary")

# Resolve the real Cargo graph. Every external dependency is scanned for unsafe
# source. Any dependency that contains unsafe/FFI/raw pointers must be explicitly
# approved by exact Cargo package name + version. This lets Qt/SDL/audio/platform
# bindings preserve C++ parity without creating a blanket unsafe exemption.
try:
    metadata = json.loads(
        subprocess.check_output(
            ["cargo", "metadata", "--format-version", "1"],
            cwd=ROOT,
            text=True,
            stderr=subprocess.STDOUT,
        )
    )
except (subprocess.CalledProcessError, FileNotFoundError, json.JSONDecodeError) as exc:
    errors.append(f"cargo metadata failed while enforcing dependency safety: {exc}")
    metadata = {"packages": []}

workspace_ids = set(metadata.get("workspace_members", []))
safe_manifest_paths = {(crate / "Cargo.toml").resolve() for crate in SAFE_CRATES}
boundary_manifest_path = boundary_manifest.resolve()
used_unsafe_exceptions: set[str] = set()

for package in metadata.get("packages", []):
    package_id = package.get("id", "")
    name = package.get("name", "")
    version = package.get("version", "")
    manifest_path = pathlib.Path(package.get("manifest_path", ""))
    package_root = manifest_path.parent

    if package_id in workspace_ids:
        resolved_manifest = manifest_path.resolve()
        if resolved_manifest == boundary_manifest_path:
            if name != BOUNDARY_NAME:
                errors.append(f"{rel(manifest_path)}: first-party unsafe boundary has unexpected package name {name!r}")
        elif resolved_manifest not in safe_manifest_paths:
            errors.append(
                f"{rel(manifest_path)}: unrecognized workspace crate; first-party crates must be safe or the dedicated {BOUNDARY_NAME} boundary"
            )
        continue

    contains_unsafe = False
    for path in sorted(package_root.rglob("*.rs")):
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        code_lines = "\n".join(line.split("//", 1)[0] for line in source.splitlines())
        if unsafe_token.search(code_lines) or extern_c.search(code_lines) or raw_pointer.search(code_lines):
            contains_unsafe = True
            break

    if not contains_unsafe:
        continue

    exception_key = f"{name}@{version}"
    if exception_key not in unsafe_allowlist:
        errors.append(
            f"dependency {exception_key!r} ({manifest_path}) contains unsafe/FFI/raw-pointer Rust and is not in the audited parity allowlist"
        )
    else:
        used_unsafe_exceptions.add(exception_key)

for exception_key in sorted(set(unsafe_allowlist) - used_unsafe_exceptions):
    errors.append(
        f"{rel(ALLOWLIST_PATH)}: stale unsafe exception {exception_key!r}; remove it unless that exact package version is in the resolved dependency graph and actually requires unsafe"
    )

if errors:
    print("Rust safety policy violations:", file=sys.stderr)
    for error in errors:
        print(f" - {error}", file=sys.stderr)
    raise SystemExit(1)

safe_count = sum(1 for crate in SAFE_CRATES for _ in crate.rglob("*.rs"))
boundary_count = sum(1 for _ in BOUNDARY.rglob("*.rs")) if BOUNDARY.exists() else 0
print(
    f"Rust safety policy OK ({safe_count} safe first-party files; "
    f"{boundary_count} file(s) in {BOUNDARY_NAME}; "
    f"{len(used_unsafe_exceptions)} audited external unsafe exception(s))"
)
