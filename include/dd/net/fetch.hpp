#pragma once

#include <cstdint>
#include <string>

namespace dd::fetch {

struct Options {
    long timeout_seconds = 30;
    long max_redirects = 5;
    std::int64_t max_body_bytes = 32 * 1024 * 1024;
    std::string user_agent = "DataDiver/0.1 (public-record research; contact site operator)";
};

// One retrieval, with the measurements the run record shows. Timings come
// from a monotonic clock around the transfer; byte counts from the body we
// actually received.
struct Result {
    std::string url;
    std::string final_url;
    bool ok = false;
    long http_status = 0;      // 0 for local files
    std::string content_type;  // as reported; empty for local files
    std::string body;
    std::int64_t bytes = 0;
    double total_ms = 0.0;
    std::string error;         // set when !ok
    std::string fetched_at;    // ISO 8601 UTC
};

// http(s) via libcurl when built with it; file:// and bare filesystem paths
// read directly so fixtures and local exports work without a network. A URL
// scheme the build cannot serve produces ok=false with a reason, never a
// fabricated body.
Result get(const std::string& url, const Options& options = {});

bool is_local(const std::string& url);
bool http_supported();

} // namespace dd::fetch
