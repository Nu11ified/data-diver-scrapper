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

Result get(const std::string& url, const Options& options = {});

enum class Mode { Api, Html, Rendered, Document };
std::string_view mode_name(Mode m);

struct Fetched {
    Result result;
    Mode mode = Mode::Html;
    bool render_attempted = false;
    std::string render_note;  // why rendering was or was not used
};

bool likely_script_rendered(const std::string& content_type, const std::string& body);

bool renderer_available();

Fetched get_auto(const std::string& url, const Options& options = {});

bool is_local(const std::string& url);
bool http_supported();
} // namespace dd::fetch
