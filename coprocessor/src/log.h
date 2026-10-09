// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// ESP-IDF logging, with a stderr fallback so portable modules build natively.

#pragma once

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <cstdio>
#define ESP_LOGE(tag, fmt, ...) \
  std::fprintf(stderr, "E %s: " fmt "\n", tag __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) \
  std::fprintf(stderr, "W %s: " fmt "\n", tag __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#endif
