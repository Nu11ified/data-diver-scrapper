#pragma once

#include <cstdint>

namespace dd::metrics {

// Resident set size of this process right now, in bytes, read from the OS.
// Returns 0 only if the platform query itself fails.
std::int64_t current_rss_bytes();

// Peak resident set size in bytes since process start, from getrusage.
std::int64_t peak_rss_bytes();

// CPU time consumed by this process (user + system), in milliseconds.
double cpu_time_ms();

} // namespace dd::metrics
