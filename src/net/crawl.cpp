#include "dd/net/crawl.hpp"

#include "dd/core/core.hpp"
#include "dd/parse/html.hpp"
#include "dd/parse/query.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <thread>

namespace dd::crawl {
namespace {
std::string strip_fragment(const std::string& url) {
    const std::size_t hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

bool starts_with_ci(const std::string& text, const char* prefix) {
    const std::string lowered = str::to_lower(text);
    return lowered.rfind(prefix, 0) == 0;
}

struct Split {
    std::string scheme;  // "https"
    std::string host;
    std::string path;    // always begins with '/'
};

bool split_url(const std::string& url, Split* out) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    out->scheme = str::to_lower(url.substr(0, scheme_end));
    const std::size_t host_start = scheme_end + 3;
    const std::size_t path_start = url.find_first_of("/?#", host_start);
    if (path_start == std::string::npos) {
        out->host = str::to_lower(url.substr(host_start));
        out->path = "/";
    } else if (url[path_start] != '/') {
        out->host = str::to_lower(url.substr(host_start, path_start - host_start));
        out->path = "/" + url.substr(path_start);
    } else {
        out->host = str::to_lower(url.substr(host_start, path_start - host_start));
        out->path = url.substr(path_start);
    }
    return !out->host.empty();
}

std::string normalize_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    const auto flush = [&] {
        if (current == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!current.empty() && current != ".") {
            parts.push_back(current);
        }
        current.clear();
    };
    std::string query;
    std::string body = path;
    const std::size_t q = body.find('?');
    if (q != std::string::npos) {
        query = body.substr(q);
        body = body.substr(0, q);
    }
    for (const char c : body) {
        if (c == '/') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    const bool trailing_slash = !body.empty() && body.back() == '/';
    flush();
    std::string out = "/" + str::join(parts, "/");
    if (trailing_slash && out.size() > 1) out += '/';
    return out + query;
}
} // namespace

std::string host_of(const std::string& url) {
    Split split;
    return split_url(url, &split) ? split.host : std::string{};
}

std::string resolve_url(const std::string& base, const std::string& href) {
    const std::string trimmed = str::trim(href);
    if (trimmed.empty() || trimmed[0] == '#') return {};
    for (const char* scheme : {"mailto:", "javascript:", "tel:", "data:", "ftp:"}) {
        if (starts_with_ci(trimmed, scheme)) return {};
    }
    if (starts_with_ci(trimmed, "http://") || starts_with_ci(trimmed, "https://")) {
        return strip_fragment(trimmed);
    }
    Split split;
    if (!split_url(base, &split)) return {};
    const std::string clean = strip_fragment(trimmed);
    if (clean.rfind("//", 0) == 0) return split.scheme + ":" + clean;
    if (!clean.empty() && clean[0] == '/') {
        return split.scheme + "://" + split.host + normalize_path(clean);
    }
    // "?page=2" replaces the query and keeps the path. Treating it as a
    // relative segment drops the page being paginated, which is the one link
    // a crawler most needs to follow.
    if (clean[0] == '?') {
        std::string path = split.path;
        const std::size_t existing = path.find('?');
        if (existing != std::string::npos) path = path.substr(0, existing);
        return split.scheme + "://" + split.host + normalize_path(path) + clean;
    }
    std::string directory = split.path;
    const std::size_t query = directory.find('?');
    if (query != std::string::npos) directory = directory.substr(0, query);
    const std::size_t slash = directory.rfind('/');
    directory = slash == std::string::npos ? "/" : directory.substr(0, slash + 1);
    return split.scheme + "://" + split.host + normalize_path(directory + clean);
}

Robots Robots::parse(const std::string& text, const std::string& agent) {
    Robots out;
    const std::string wanted = str::to_lower(agent);
    // Records for our agent win outright; the wildcard record is the fallback.
    std::vector<Rule> wildcard;
    std::vector<Rule> specific;
    double wildcard_delay = 0.0;
    double specific_delay = 0.0;
    bool in_wildcard = false;
    bool in_specific = false;
    bool previous_was_agent = false;

    for (const std::string& raw : str::split(text, '\n')) {
        std::string line = raw;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        line = str::trim(line);
        if (line.empty()) continue;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = str::to_lower(str::trim(line.substr(0, colon)));
        const std::string value = str::trim(line.substr(colon + 1));

        if (key == "user-agent") {
            const std::string who = str::to_lower(value);
            if (!previous_was_agent) {
                in_wildcard = false;
                in_specific = false;
            }
            if (who == "*") in_wildcard = true;
            if (!who.empty() && who != "*" && wanted.find(who) != std::string::npos) {
                in_specific = true;
            }
            previous_was_agent = true;
            continue;
        }
        previous_was_agent = false;

        if (key == "disallow" || key == "allow") {
            const bool allow = key == "allow";
            // "Disallow:" with no path is an explicit allow-all for that agent.
            if (value.empty() && !allow) continue;
            if (in_specific) specific.push_back(Rule{value, allow});
            if (in_wildcard) wildcard.push_back(Rule{value, allow});
        } else if (key == "crawl-delay") {
            try {
                const double delay = std::stod(value);
                if (in_specific) specific_delay = delay;
                if (in_wildcard) wildcard_delay = delay;
            } catch (const std::exception&) {
                // an unparseable delay is not a reason to refuse the site
            }
        }
    }

    if (!specific.empty() || specific_delay > 0.0) {
        out.rules_ = std::move(specific);
        out.crawl_delay_ = specific_delay;
    } else {
        out.rules_ = std::move(wildcard);
        out.crawl_delay_ = wildcard_delay;
    }
    return out;
}

bool Robots::allowed(const std::string& path) const {
    std::size_t best = 0;
    bool decision = true;
    for (const Rule& rule : rules_) {
        if (rule.path.empty() || path.rfind(rule.path, 0) != 0) continue;
        // Longest match wins; Allow beats Disallow at equal length.
        if (rule.path.size() > best || (rule.path.size() == best && rule.allow)) {
            best = rule.path.size();
            decision = rule.allow;
        }
    }
    return decision;
}

std::vector<std::string> links_from(const std::string& url, const std::string& html_text,
                                    std::size_t limit) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    const html::Document document = html::parse(html_text);
    std::vector<const html::Node*> anchors;
    try {
        anchors = html::query_all(document, "a[href]");
    } catch (const Error&) {
        return out;
    }

    // Pagination first: a county results table is usually spread over pages,
    // and the deeper record links repeat on every one of them.
    std::vector<const html::Node*> ordered;
    std::vector<const html::Node*> rest;
    for (const html::Node* anchor : anchors) {
        const std::string text = str::to_lower(str::collapse_ws(anchor->text_content()));
        const std::string* rel = anchor->attr("rel");
        const bool paginates =
            text == "next" || text == "next page" || text == ">" || text == "»" ||
            (rel != nullptr && str::to_lower(*rel) == "next") ||
            (!text.empty() && text.size() <= 3 &&
             std::all_of(text.begin(), text.end(),
                         [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }));
        (paginates ? ordered : rest).push_back(anchor);
    }
    ordered.insert(ordered.end(), rest.begin(), rest.end());

    for (const html::Node* anchor : ordered) {
        if (out.size() >= limit) break;
        const std::string* href = anchor->attr("href");
        if (href == nullptr) continue;
        const std::string resolved = resolve_url(url, *href);
        if (resolved.empty()) continue;
        if (!seen.insert(resolved).second) continue;
        out.push_back(resolved);
    }
    return out;
}

Stats crawl(const std::string& seed, const Options& options,
            const std::function<bool(const Page&)>& visit,
            const std::function<void(const std::string&)>& log) {
    Stats stats;
    const std::string seed_host = host_of(seed);
    if (seed_host.empty()) {
        stats.failed = 1;
        if (log) log("crawl: seed is not an absolute http(s) url");
        return stats;
    }

    fetch::Options net;
    net.timeout_seconds = options.timeout_seconds;
    net.user_agent = options.user_agent;

    Robots robots = Robots::allow_all();
    if (options.obey_robots) {
        Split split;
        split_url(seed, &split);
        const fetch::Result rules =
            fetch::get(split.scheme + "://" + split.host + "/robots.txt", net);
        if (rules.ok && rules.http_status == 200) {
            robots = Robots::parse(rules.body, options.user_agent);
            stats.robots_loaded = true;
            if (log) log("crawl: robots.txt loaded");
        } else if (log) {
            log("crawl: no robots.txt served; proceeding");
        }
    }

    const long delay_ms =
        std::max<long>(options.politeness_ms,
                       static_cast<long>(robots.crawl_delay_seconds() * 1000.0));

    std::deque<std::pair<std::string, std::size_t>> queue{{strip_fragment(seed), 0}};
    std::set<std::string> queued{strip_fragment(seed)};
    std::set<std::string> body_hashes;

    while (!queue.empty() && stats.fetched < options.max_pages) {
        const auto [url, depth] = queue.front();
        queue.pop_front();

        Split split;
        if (!split_url(url, &split)) continue;
        if (options.same_host && split.host != seed_host) {
            ++stats.skipped_offsite;
            continue;
        }
        if (options.obey_robots && !robots.allowed(split.path)) {
            ++stats.skipped_robots;
            if (log) log("robots refuses " + split.path);
            continue;
        }

        if (stats.fetched > 0 && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        const fetch::Result got = fetch::get(url, net);
        if (!got.ok) {
            ++stats.failed;
            if (log) log("failed " + url + ": " + got.error);
            continue;
        }
        stats.fetch_ms += got.total_ms;

        // The same table served under two urls is one page, not two.
        const std::string digest = std::to_string(std::hash<std::string>{}(got.body));
        if (!body_hashes.insert(digest).second) {
            ++stats.skipped_duplicate;
            continue;
        }

        ++stats.fetched;
        Page page;
        page.url = got.final_url.empty() ? url : got.final_url;
        page.content_type = got.content_type;
        page.body = got.body;
        page.depth = depth;
        page.bytes = got.bytes;
        page.fetch_ms = got.total_ms;
        if (!visit(page)) {
            if (log) log("crawl: caller stopped after " + std::to_string(stats.fetched) + " pages");
            break;
        }

        if (depth >= options.max_depth) continue;
        if (str::to_lower(got.content_type).find("html") == std::string::npos &&
            !got.content_type.empty()) {
            continue;  // only markup carries links worth following
        }
        for (const std::string& link : links_from(page.url, got.body, options.max_links_per_page)) {
            if (queued.size() >= options.max_pages * 8) break;
            if (options.same_host && host_of(link) != seed_host) continue;
            if (queued.insert(link).second) queue.emplace_back(link, depth + 1);
        }
    }
    return stats;
}
} // namespace dd::crawl
