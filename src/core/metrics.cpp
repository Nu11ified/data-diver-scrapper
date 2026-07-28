#include "dd/core/metrics.hpp"

#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace dd::metrics {

std::int64_t peak_rss_bytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::int64_t>(usage.ru_maxrss); // bytes on Darwin
#else
    return static_cast<std::int64_t>(usage.ru_maxrss) * 1024; // kilobytes on Linux
#endif
}

std::int64_t current_rss_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const kern_return_t rc = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                       reinterpret_cast<task_info_t>(&info), &count);
    if (rc != KERN_SUCCESS) return 0;
    return static_cast<std::int64_t>(info.resident_size);
#else
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    long total = 0;
    long resident = 0;
    const int scanned = std::fscanf(f, "%ld %ld", &total, &resident);
    std::fclose(f);
    if (scanned != 2) return 0;
    return static_cast<std::int64_t>(resident) * sysconf(_SC_PAGESIZE);
#endif
}

double cpu_time_ms() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
    const double user =
        static_cast<double>(usage.ru_utime.tv_sec) * 1000.0 +
        static_cast<double>(usage.ru_utime.tv_usec) / 1000.0;
    const double sys =
        static_cast<double>(usage.ru_stime.tv_sec) * 1000.0 +
        static_cast<double>(usage.ru_stime.tv_usec) / 1000.0;
    return user + sys;
}

} // namespace dd::metrics
