# T-Encoder Workspace Justfile
#
# Modular workspace structure:
#  1. Host workspace: `portable`
#  2. Device workspace: `esp32-devices` (aliased as `device`)
#  3. Co-processor workspace: `coprocessor`

set shell := ["bash", "-uc"]
set unstable := true
set lists := true
set dotenv-load := true
set dotenv-filename := [".env", ".env.local"]
set dotenv-override := true

# Portable workspace. Including app simulator with MCP. Aliased as `host`.
[group("sub-modules")]
mod portable

# ESP32 device-specific workspace. Aliased as `device`.
[group("sub-modules")]
mod esp32-devices

# Co-processor C++ PlatformIO workspace.
[group("sub-modules")]
mod coprocessor

alias device := esp32-devices
alias host := portable

_default:
    @just --list

# -----------------------------------------------------------------------------
# FlatBuffers Schema
# -----------------------------------------------------------------------------

[doc("Compile FlatBuffers schemas into C++ headers and Rust bindings (make-driven incremental).")]
[group("codegen")]
schema:
    make -C schemas

# -----------------------------------------------------------------------------
# Parallel Aggregates
# -----------------------------------------------------------------------------

[doc("Format all Rust and C++ files across all subprojects.")]
[group("verification")]
[parallel]
fmt: portable::fmt esp32-devices::fmt coprocessor::fmt

[doc("Check formatting without modifying files across all subprojects.")]
[group("verification")]
[parallel]
fmt-check: portable::fmt-check esp32-devices::fmt-check coprocessor::fmt-check

[doc("Remove build artifacts and formatting/lint stamps from all subprojects.")]
[group("maintenance")]
[parallel]
clean: portable::clean esp32-devices::clean coprocessor::clean

[doc("Typecheck both portable workspace crates and device firmware in parallel.")]
[group("verification")]
[parallel]
check: portable::check esp32-devices::check

[doc("Run all linters in parallel (portable clippy, device clippy, coprocessor clang-tidy).")]
[group("verification")]
[parallel]
lint: portable::lint esp32-devices::lint coprocessor::tidy

[doc("Run host unit tests across portable crates and co-processor.")]
[group("verification")]
test *ARGS:
    just portable::test {{ARGS}}
    just coprocessor::test {{ARGS}}

# -----------------------------------------------------------------------------
# Verification Suite
# -----------------------------------------------------------------------------

[doc("Run fast verification: format, typecheck, lint, test, and coprocessor build.")]
[group("verification")]
verify: fmt check lint test coprocessor::build

[doc("Run verify plus full release builds for all hardware targets.")]
[group("verification")]
verify-all: verify (esp32-devices::build "waveshare_knob_1_8") (esp32-devices::build "lilygo_t_encoder_pro")

# -----------------------------------------------------------------------------
# Diagnostics & Info
# -----------------------------------------------------------------------------

[doc("Print toolchain and environment diagnostic info.")]
[group("debug")]
env-info:
    @echo "=== Host Toolchain ==="
    @rustc --version
    @echo ""
    @echo "=== Device Toolchain (esp32-devices) ==="
    @cd esp32-devices && rustc --version
