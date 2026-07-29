#include "dd/net/fetch.hpp"

#include "dd/core/core.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(DD_HAVE_CURL)
#include <curl/curl.h>
#include <mutex>
#endif

namespace dd::fetch {
namespace {
bool has_prefix(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string local_path(const std::string& url) {
    if (has_prefix(url, "file://")) return url.substr(7);
    return url;
}

Result fetch_local(const std::string& url, const Options& options) {
    Result r;
    r.url = url;
    r.final_url = url;
    r.fetched_at = timeutil::iso_now();
    const Stopwatch watch;
    try {
        r.body = fileio::read_file(local_path(url));
        if (static_cast<std::int64_t>(r.body.size()) > options.max_body_bytes) {
            r.body.clear();
            r.error = "local file exceeds max_body_bytes";
        } else {
            r.ok = true;
        }
    } catch (const Error& e) {
        r.error = e.what();
    }
    r.total_ms = watch.elapsed_ms();
    r.bytes = static_cast<std::int64_t>(r.body.size());
    return r;
}

#if defined(DD_HAVE_CURL)

std::size_t write_body(char* data, std::size_t size, std::size_t nmemb, void* user) {
    auto* pair = static_cast<std::pair<std::string*, std::int64_t>*>(user);
    const std::size_t total = size * nmemb;
    if (static_cast<std::int64_t>(pair->first->size() + total) > pair->second) {
        return 0; // Abort the transfer: body over limit.
    }
    pair->first->append(data, total);
    return total;
}

void global_init_once() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string encode_spaces(const std::string& url) {
    return str::replace_all(url, " ", "%20");
}

Result fetch_http(const std::string& url, const Options& options) {
    global_init_once();
    Result r;
    r.url = url;
    r.final_url = url;
    r.fetched_at = timeutil::iso_now();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        r.error = "curl_easy_init failed";
        return r;
    }

    std::pair<std::string*, std::int64_t> sink{&r.body, options.max_body_bytes};
    char error_buffer[CURL_ERROR_SIZE] = {0};

    const std::string request_url = encode_spaces(url);
    curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, options.max_redirects);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, options.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, options.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, options.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // ask for gzip and let curl inflate

    const Stopwatch watch;
    const CURLcode rc = curl_easy_perform(curl);
    r.total_ms = watch.elapsed_ms();
    r.bytes = static_cast<std::int64_t>(r.body.size());

    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        r.http_status = status;
        char* effective = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
        if (effective != nullptr) r.final_url = effective;
        char* type = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
        if (type != nullptr) r.content_type = type;
        if (status >= 200 && status < 300) {
            r.ok = true;
        } else {
            r.error = "http status " + std::to_string(status);
        }
    } else {
        r.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(rc);
    }
    curl_easy_cleanup(curl);
    return r;
}

#endif // DD_HAVE_CURL
} // namespace

bool is_local(const std::string& url) {
    if (has_prefix(url, "file://")) return true;
    return !str::contains(url, "://");
}

bool http_supported() {
#if defined(DD_HAVE_CURL)
    return true;
#else
    return false;
#endif
}

Result fetch_rendered(const std::string& url, const Options& options) {
    Result r;
    r.url = url;
    r.final_url = url;
    r.fetched_at = timeutil::iso_now();
    const std::string target = url.substr(7); // after "render+"
    if (!has_prefix(target, "http://") && !has_prefix(target, "https://")) {
        r.error = "render+ requires an http(s) url";
        return r;
    }
    if (target.find('\'') != std::string::npos || target.find('\n') != std::string::npos) {
        r.error = "render+ url contains characters the renderer command cannot take";
        return r;
    }
#if defined(__EMSCRIPTEN__)
    r.error = "render+ is unavailable in the WASM build; the host fetches and renders";
    return r;
#else
    const char* renderer = std::getenv("DD_RENDERER");
    if (renderer == nullptr || renderer[0] == '\0') {
        r.error = "set DD_RENDERER to a command that prints rendered HTML for a url "
                  "(e.g. 'npx tsx tools/render.ts', see the Makefile)";
        return r;
    }
    const Stopwatch watch;
    const std::string command = std::string{renderer} + " '" + target + "'";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        r.error = "could not start renderer: " + std::string{renderer};
        return r;
    }
    char buffer[1 << 14];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        r.body.append(buffer, n);
        if (static_cast<std::int64_t>(r.body.size()) > options.max_body_bytes) break;
    }
    const int status = pclose(pipe);
    r.total_ms = watch.elapsed_ms();
    r.bytes = static_cast<std::int64_t>(r.body.size());
    r.content_type = "text/html";
    if (status != 0) {
        r.body.clear();
        r.bytes = 0;
        r.error = "renderer exited with status " + std::to_string(status);
    } else if (r.body.empty()) {
        r.error = "renderer produced no output";
    } else {
        r.ok = true;
    }
    return r;
#endif
}

Result get(const std::string& url, const Options& options) {
    if (has_prefix(url, "render+")) return fetch_rendered(url, options);
    if (is_local(url)) return fetch_local(url, options);
    if (has_prefix(url, "http://") || has_prefix(url, "https://")) {
#if defined(DD_HAVE_CURL)
        return fetch_http(url, options);
#else
        Result r;
        r.url = url;
        r.final_url = url;
        r.fetched_at = timeutil::iso_now();
        r.error = "engine built without libcurl: cannot fetch http(s)";
        return r;
#endif
    }
    Result r;
    r.url = url;
    r.final_url = url;
    r.fetched_at = timeutil::iso_now();
    r.error = "unsupported url scheme";
    return r;
}

std::string_view mode_name(Mode m) {
    switch (m) {
    case Mode::Api: return "api";
    case Mode::Html: return "html";
    case Mode::Rendered: return "rendered";
    case Mode::Document: return "document";
    }
    return "html";
}

bool renderer_available() {
#if defined(__EMSCRIPTEN__)
    return false;
#else
    const char* renderer = std::getenv("DD_RENDERER");
    return renderer != nullptr && renderer[0] != '\0';
#endif
}

bool likely_script_rendered(const std::string& content_type, const std::string& body) {
    const std::string type = str::to_lower(content_type);
    const std::string head = str::to_lower(body.substr(0, std::min<std::size_t>(body.size(), 4096)));
    const bool html = str::contains(type, "html") ||
                      (type.empty() && (str::contains(head, "<!doctype html") ||
                                        str::contains(head, "<html")));
    if (!html || body.empty()) return false;

    static const char* kShells[] = {
        "id=\"root\"", "id=\"app\"", "id=\"__next\"", "__next_data__", "ng-app",
        "data-reactroot", "window.__initial_state__", "id=\"react-root\"",
    };
    const std::string lowered = str::to_lower(body);
    bool shell = false;
    for (const char* marker : kShells) {
        if (str::contains(lowered, marker)) shell = true;
    }

    std::size_t script_bytes = 0;
    std::size_t at = 0;
    while ((at = lowered.find("<script", at)) != std::string::npos) {
        const std::size_t close = lowered.find("</script>", at);
        if (close == std::string::npos) break;
        script_bytes += close - at;
        at = close + 9;
    }
    std::size_t text_bytes = 0;
    bool in_tag = false;
    for (char c : body) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag && std::isspace(static_cast<unsigned char>(c)) == 0) ++text_bytes;
    }
    const bool code_heavy = text_bytes * 4 < script_bytes;
    return shell || code_heavy;
}

Fetched get_auto(const std::string& url, const Options& options) {
    Fetched out;
    out.result = get(url, options);
    if (has_prefix(url, "render+")) {
        out.mode = Mode::Rendered;
        out.render_attempted = true;
        return out;
    }
    if (!out.result.ok) return out;

    const std::string type = str::to_lower(out.result.content_type);
    if (str::contains(type, "json") || str::contains(type, "csv") ||
        str::contains(type, "xml")) {
        out.mode = Mode::Api;
        return out;
    }
    if (str::contains(type, "pdf")) {
        out.mode = Mode::Document;
        return out;
    }
    if (is_local(url)) return out;

    if (!likely_script_rendered(out.result.content_type, out.result.body)) return out;
    if (!renderer_available()) {
        out.render_note = "looks script-rendered; set DD_RENDERER to fetch it through a browser";
        return out;
    }
    out.render_attempted = true;
    Fetched rendered;
    rendered.result = get("render+" + url, options);
    if (!rendered.result.ok) {
        out.render_note = "render attempt failed: " + rendered.result.error;
        return out;
    }
    rendered.mode = Mode::Rendered;
    rendered.render_attempted = true;
    rendered.result.url = url;
    rendered.render_note = "static body was a framework shell";
    return rendered;
}
} // namespace dd::fetch
