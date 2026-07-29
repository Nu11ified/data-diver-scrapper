#pragma once

#include "dd/net/fetch.hpp"

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace dd::crawl {
/// Resolves a possibly relative href against the page it was found on.
/// Empty when the link is unusable: a fragment, a mailto, javascript, or a
/// scheme this crawler will not follow.
std::string resolve_url(const std::string& base, const std::string& href);

std::string host_of(const std::string& url);

/// The subset of robots.txt that governs whether a path may be fetched:
/// the longest matching Disallow wins, an Allow of equal or greater length
/// overrides it, and a record for our agent beats the wildcard record.
class Robots {
public:
    static Robots parse(const std::string& text, const std::string& agent);
    static Robots allow_all() { return Robots{}; }

    bool allowed(const std::string& path) const;
    double crawl_delay_seconds() const { return crawl_delay_; }

private:
    struct Rule {
        std::string path;
        bool allow = false;
    };
    std::vector<Rule> rules_;
    double crawl_delay_ = 0.0;
};

struct Options {
    std::size_t max_pages = 40;
    std::size_t max_depth = 2;
    bool same_host = true;
    long politeness_ms = 400;   // floor between requests to one host
    bool obey_robots = true;
    long timeout_seconds = 20;
    std::size_t max_links_per_page = 200;
    std::string user_agent = "DataDiver/0.1 (public-record research; contact site operator)";
};

struct Page {
    std::string url;
    std::string content_type;
    std::string body;
    std::size_t depth = 0;
    std::int64_t bytes = 0;
    double fetch_ms = 0.0;
};

struct Stats {
    std::size_t fetched = 0;
    std::size_t skipped_duplicate = 0;   // same URL, or same body as a page already seen
    std::size_t skipped_robots = 0;
    std::size_t skipped_offsite = 0;
    std::size_t failed = 0;
    double fetch_ms = 0.0;
    bool robots_loaded = false;
};

/// Follows links from a seed, breadth first, and hands every fetched page to
/// `visit`. Returning false from `visit` stops the crawl, which is how a
/// caller stops once it has the records it came for.
Stats crawl(const std::string& seed, const Options& options,
            const std::function<bool(const Page&)>& visit,
            const std::function<void(const std::string&)>& log = {});

/// Links worth following from one page, in document order and already
/// resolved. Pagination is preferred over everything else: a results table
/// spread across pages is the common county shape.
std::vector<std::string> links_from(const std::string& url, const std::string& html,
                                    std::size_t limit);
} // namespace dd::crawl
