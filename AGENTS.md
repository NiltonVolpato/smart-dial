# Workspace Command Execution (Always Use Just)

Do NOT run raw `cargo`, `idf.py`, `pio`, or manual build tools directly in the shell. Toolchains, target architectures, environment variables, and build configurations are strictly orchestrated via `just`.
- Run `just --list` to discover all available recipes across the root workspace and sub-modules.
- Use `just test` to run the test suite across portable crates and the co-processor.
- Use `just check` to typecheck portable crates and device firmware in parallel.
- Use `just lint` to run linters (clippy, clang-tidy).
- Use `just fmt` / `just fmt-check` for code formatting.
- Specific sub-modules can be invoked via recipes like `just portable::test`, `just device::check`, `just coprocessor::build`, etc.

---

# API Visibility & Exports

- **Most private visibility**: Default to private or `pub(crate)`. Never expose internals with `pub`.
- **One canonical name**: No duplicate re-exports. Export each public symbol once. Don't re-export foreign crate types.
- **Flat namespace**: For simple crates, export directly from crate root and keep modules private. At most one submodule level for complex crates.
- **Only used APIs**: Every public symbol must be intentionally designed and actively consumed. Privatize or remove unused items.

---

# Project Environment & Repository Paths

This file maintains key directory paths and architectures across related worktrees and subprojects for agent navigation.

## Related Worktrees & Repositories

- **Tether RPC Framework**: `/Users/nilton/Code/tether-worktree/main`
  - **Schema**: `/Users/nilton/Code/tether-worktree/main/schema/` (e.g. `status.fbs`, `tether.fbs`)
  - **C++ Client/Server Library**: `/Users/nilton/Code/tether-worktree/main/cpp/` (headers in `cpp/tether/include/tether/`: `typed.h`, `server.h`, `client.h`, `frame.h`)
  - **Rust Crates**:
    - `tether-core`: `/Users/nilton/Code/tether-worktree/main/core/` (framing, zero-copy buffer pools, wire status)
    - `tether-embassy`: `/Users/nilton/Code/tether-worktree/main/embassy/` (Embassy async client runner: `run_client`)
- **Slint Fork**: `/Users/nilton/Code/smart-dial-worktree/forks/slint`
  - Core crate: `internal/core/`
  - Image & pixel buffers: `internal/core/graphics/image.rs` (`SharedPixelBuffer`, `SharedVector`)

## Smart Dial Workspace Structure

- **Coprocessor (ESP32)**: `coprocessor/`
  - Services: `coprocessor/src/` (`sonos_service.cpp`, `sonos_controller.cpp`, `wifi_service.cpp`, `main.cpp`)
  - Headers: `coprocessor/include/`
  - Build config: `coprocessor/platformio.ini`, `coprocessor/CMakeLists.txt`
- **Device Firmware (ESP32-S3)**: `esp32-devices/waveshare_knob_1_8/`
  - Entry point: `src/bin/main.rs` (Core 0 Embassy RTOS + Core 1 AppCpu display/rendering)
  - Coprocessor BSP: `src/bsp/coprocessor.rs` (High-priority interrupt UART RX ring buffer, client runner)
  - Display driver: `src/bsp/display.rs` (SH8601 QSPI DMA)
- **Portable Crates**: `portable/crates/`
  - `coprocessor`: Auto-generated flatbuffers & typed bindings for Tether services
  - `app-shell`: Headless lifecycle manager, event routing, async task scheduler
  - `jpegdec`: C++ bindings to Larry Bank's fast JPEGDEC decoder
- **Applications**: `portable/apps/`
  - `sonos`: Sonos controller UI (`ui/sonos.slint`) & state management (`src/lib.rs`)
  - `launcher`: App launcher carousel
  - `clock`, `macropad`, `pomodoro`, `simon`, `magic8`
