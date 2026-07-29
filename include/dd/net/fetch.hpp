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
// "render+https://..." URLs are fetched through an external renderer (a
// headless browser) named by the DD_RENDERER environment variable: the
// command is run with the URL as its argument and its stdout becomes the
// body. The engine takes bytes from it and nothing else, so parsing,
// classification and extraction stay in-process.
Result get(const std::string& url, const Options& options = {});

// How a body arrived, reported so the CLI can show which path a source took.
enum class Mode { Api, Html, Rendered, Document };
std::string_view mode_name(Mode m);

struct Fetched {
    Result result;
    Mode mode = Mode::Html;
    bool render_attempted = false;
    std::string render_note;  // why rendering was or was not used
};

// A body is script-rendered when the markup is a framework shell: an app
// mount point or hydration payload, and far more script than visible text.
bool likely_script_rendered(const std::string& content_type, const std::string& body);

bool renderer_available();

// One retrieval that handles every source shape without the caller knowing
// which it is: JSON and CSV come back as they are, static HTML likewise, and
// a framework shell is re-fetched through the renderer when one is
// configured. The chosen path is reported, never guessed at silently.
Fetched get_auto(const std::string& url, const Options& options = {});

bool is_local(const std::string& url);
bool http_supported();

} // namespace dd::fetch
