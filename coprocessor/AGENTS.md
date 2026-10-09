# coprocessor

## Rules
- Write modern, maintenable, well architectured C++ code, not C.
- When interacting with C libraries, upgrade from C to C++ on the first opportunity (if possible automatically).
  - For example: convert nul terminated strings to `std::string_view`, wrap raw pointers in smart pointers to avoid memory leaks, etc

## Architecture Decision Record (ADRs)

