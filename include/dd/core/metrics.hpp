#pragma once

#include <cstdint>

namespace dd::metrics {
std::int64_t current_rss_bytes();

std::int64_t peak_rss_bytes();

double cpu_time_ms();
} // namespace dd::metrics
