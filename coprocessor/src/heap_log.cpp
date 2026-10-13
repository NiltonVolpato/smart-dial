// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "heap_log.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "xml_query/xml_query.h"

namespace coprocessor {

void log_heap(const char* where) {
  if (esp_log_level_get(kHeapTag) < ESP_LOG_DEBUG) {
    return;  // Skips walking the heap for the largest block.
  }
  ESP_LOGD(
      kHeapTag, "%s: free=%u largest=%u min=%u xq=%u (peak %u)", where,
      static_cast<unsigned>(esp_get_free_heap_size()),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(esp_get_minimum_free_heap_size()),
      static_cast<unsigned>(xq::memory_in_use()),
      static_cast<unsigned>(xq::peak_memory_in_use()));
  xq::reset_peak_memory();
}

}  // namespace coprocessor
