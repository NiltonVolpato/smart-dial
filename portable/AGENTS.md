# Portable Crates & Apps Architecture

## Architecture Decision Records (ADRs)

### ADR-01: Maximize Crate Reuse Across Host & Target
- **Decision**: Builds targeting the host must reuse the exact same `no_std` crates used on embedded devices whenever those crates support the host. Only introduce host-specific alternatives or portable abstraction layers when embedded crates are fundamentally incompatible with host platforms.
- **Rule**: Do not reinvent abstractions or write custom queues, fake clocks, or synchronization primitives when portable crates (e.g., `embassy-sync`, `embassy-futures`, `embassy-executor`, `jiff`) already provide standard, efficient implementations for both host and target.

### ADR-02: Zero Global Static State & Strict Dependency Injection
- **Decision**: All application state, platform services, and lifecycle coordinators must be passed down explicitly via dependency injection (e.g., `ShellContext`, `Rc<dyn Platform>`).
- **Rule**: Never use global static variables (`static Mutex<...>`, `static RefCell<...>`) for application or platform state. Never implement `unsafe impl Send` or `unsafe impl Sync` (e.g., `SendSyncWrapper`) to bypass thread-safety checks on `!Send` types like Slint UI handles (`Weak<T>`).

### ADR-03: Idiomatic RAII Lifecycle & Automatic Cleanup
- **Decision**: Resource lifecycles, background tasks, and event subscriptions must be governed strictly by Rust's RAII principles, weak references, and drop semantics.
- **Rule**: Bind resource lifecycles (timers, background futures, communication handles) directly to the owning instance (e.g., the handle returned by `AppFactory::launch`). Dropping an instance must automatically trigger cancellation and release all associated resources. Avoid manual lifecycle registries or manual cleanup calls when `Drop` can guarantee cleanup.
