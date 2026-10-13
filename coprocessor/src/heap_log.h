// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

namespace coprocessor {

inline constexpr const char* kHeapTag = "heap";

#ifdef ESP_PLATFORM
/// Logs the heap at `where`: free, the largest free block, the low-water
/// mark, and what Expat holds now and at most since the last log (xq). Debug
/// level, under kHeapTag: off unless raised, e.g. through the web server's
/// /api/log. Any task.
void log_heap(const char* where);
#else
inline void log_heap(const char* /*where*/) {}
#endif

}  // namespace coprocessor
