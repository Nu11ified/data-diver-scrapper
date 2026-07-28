#include "dd/net/server.hpp"

#include "dd/ml/classify.hpp"
#include "dd/core/core.hpp"
#include "dd/engine/events.hpp"
#include "dd/net/fetch.hpp"
#include "dd/core/json.hpp"
#include "dd/core/metrics.hpp"

#include <cerrno>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace dd::server {
namespace {

struct Request {
    std::string method;
    std::string path; // without query string
    std::map<std::string, std::string> query;
    std::string body;
};

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '+') {
            out.push_back(' ');
            continue;
        }
        if (c == '%' && i + 2 < s.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

std::map<std::string, std::string> parse_query(std::string_view raw) {
    std::map<std::string, std::string> out;
    std::size_t start = 0;
    while (start < raw.size()) {
        std::size_t end = raw.find('&', start);
        if (end == std::string_view::npos) end = raw.size();
        const std::string_view pair = raw.substr(start, end - start);
        const std::size_t eq = pair.find('=');
        if (eq == std::string_view::npos) {
            out[url_decode(pair)] = "";
        } else {
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        start = end + 1;
    }
    return out;
}

std::string status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    }
    return "OK";
}

void send_all(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

void respond(int fd, int code, std::string_view content_type, std::string_view body) {
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + status_text(code) + "\r\n";
    head += "Content-Type: " + std::string{content_type} + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Connection: close\r\n\r\n";
    send_all(fd, head);
    send_all(fd, body);
}

void respond_json(int fd, int code, std::string_view body) {
    respond(fd, code, "application/json; charset=utf-8", body);
}

void respond_error(int fd, int code, std::string_view message) {
    json::Writer w;
    w.begin_object();
    w.field("error", message);
    w.end_object();
    respond_json(fd, code, w.str());
}

void write_run(json::Writer& w, const store::RunRecord& r) {
    w.raw_value(r.serialize());
}

void write_source(json::Writer& w, const store::Source& s, const store::SourceState& state,
                  const std::vector<store::RunRecord>& last_runs) {
    w.begin_object();
    w.field("id", s.id);
    w.field("name", s.name);
    w.field("url", s.url);
    w.field("jurisdiction", s.jurisdiction);
    w.field("added_at", s.added_at);
    w.field("enabled", s.enabled);
    w.field("classification", state.classification);
    w.field("has_mapping", state.has_mapping);
    w.field("mapping_confidence", state.has_mapping ? state.mapping.confidence : 0.0);
    w.field("baseline_rate", state.baseline_rate);
    w.field("good_runs", state.good_runs);
    if (state.has_mapping) {
        w.field_raw("mapping", state.mapping.serialize());
    } else {
        w.key("mapping");
        w.null_value();
    }
    w.key("last_run");
    if (last_runs.empty()) {
        w.null_value();
    } else {
        write_run(w, last_runs.front());
    }
    w.end_object();
}

} // namespace

Server::Server(store::Store& store, pipeline::Pipeline& pipeline, Options options)
    : store_{store}, pipeline_{pipeline}, options_{std::move(options)} {}

Server::~Server() { stop(); }

void Server::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw Error("server: cannot create socket");

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(options_.port));
    if (::inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1) {
        throw Error("server: bad host " + options_.host);
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string why = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw Error("server: cannot bind " + options_.host + ":" +
                    std::to_string(options_.port) + ": " + why);
    }
    if (::listen(listen_fd_, 16) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw Error("server: listen failed");
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len);
    bound_port_ = ntohs(bound.sin_port);

    running_ = true;
    accept_thread_ = std::thread{[this] { accept_loop(); }};
    logging::info("server: listening on http://" + options_.host + ":" +
                  std::to_string(bound_port_));
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

void Server::accept_loop() {
    while (running_) {
        const int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            if (running_) continue;
            break;
        }
        std::thread{[this, client] {
            try {
                handle_connection(client);
            } catch (const std::exception& e) {
                respond_error(client, 500, e.what());
            }
            ::close(client);
        }}.detach();
    }
}

void Server::handle_connection(int client_fd) {
    // Read headers.
    std::string data;
    char buffer[8192];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        data.append(buffer, static_cast<std::size_t>(n));
        header_end = data.find("\r\n\r\n");
        if (data.size() > 1 << 20) {
            respond_error(client_fd, 400, "headers too large");
            return;
        }
    }

    Request req;
    {
        const std::string head = data.substr(0, header_end);
        const std::vector<std::string> lines = str::split(head, '\n');
        const std::vector<std::string> parts = str::split(str::trim(lines[0]), ' ');
        if (parts.size() < 2) {
            respond_error(client_fd, 400, "malformed request line");
            return;
        }
        req.method = parts[0];
        std::string target = parts[1];
        const std::size_t qmark = target.find('?');
        if (qmark != std::string::npos) {
            req.query = parse_query(std::string_view{target}.substr(qmark + 1));
            target = target.substr(0, qmark);
        }
        req.path = url_decode(target);

        std::size_t content_length = 0;
        for (const std::string& line : lines) {
            const std::string lowered = str::to_lower(line);
            if (lowered.rfind("content-length:", 0) == 0) {
                content_length = static_cast<std::size_t>(
                    std::atoll(str::trim(line.substr(15)).c_str()));
            }
        }
        if (content_length > (8u << 20)) {
            respond_error(client_fd, 400, "body too large");
            return;
        }
        req.body = data.substr(header_end + 4);
        while (req.body.size() < content_length) {
            const ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            req.body.append(buffer, static_cast<std::size_t>(n));
        }
    }

    // ------------------------------------------------------------ routes ---
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        const std::string page_path = options_.web_root + "/index.html";
        if (!fileio::exists(page_path)) {
            respond_error(client_fd, 404, "ui not found at " + page_path);
            return;
        }
        respond(client_fd, 200, "text/html; charset=utf-8", fileio::read_file(page_path));
        return;
    }

    if (req.path == "/api/overview" && req.method == "GET") {
        const std::vector<store::Source> sources = store_.sources();
        const std::vector<store::RunRecord> all_runs = store_.runs(100000);

        json::Writer w;
        w.begin_object();
        w.key("engine");
        w.begin_object();
        w.field("rss_bytes", metrics::current_rss_bytes());
        w.field("peak_rss_bytes", metrics::peak_rss_bytes());
        w.field("cpu_ms", metrics::cpu_time_ms());
        w.field("http_transport", fetch::http_supported());
        w.field("now", timeutil::iso_now());
        w.end_object();
        w.key("model");
        w.begin_object();
        w.field("trained_at", pipeline_.classifier().trained_at());
        w.field("leave_one_out_accuracy", pipeline_.classifier().trained_accuracy());
        w.field("examples", static_cast<std::int64_t>(pipeline_.classifier().example_count()));
        w.end_object();
        w.key("totals");
        w.begin_object();
        w.field("sources", static_cast<std::int64_t>(sources.size()));
        w.field("runs", static_cast<std::int64_t>(all_runs.size()));
        w.field("events", static_cast<std::int64_t>(store_.event_count()));
        w.field("properties", static_cast<std::int64_t>(store_.property_keys().size()));
        w.field("repairs", static_cast<std::int64_t>(store_.repairs().size()));
        w.end_object();

        // Per-source aggregates, measured from run records.
        w.key("per_source");
        w.begin_array();
        for (const store::Source& s : sources) {
            std::size_t runs = 0;
            std::size_t ok = 0;
            double total_ms_sum = 0.0;
            double fetch_ms_sum = 0.0;
            std::int64_t bytes_sum = 0;
            const store::RunRecord* last = nullptr;
            for (const store::RunRecord& r : all_runs) {
                if (r.source_id != s.id) continue;
                ++runs;
                if (r.ok) ++ok;
                total_ms_sum += r.total_ms;
                fetch_ms_sum += r.fetch_ms;
                bytes_sum += r.bytes;
                if (last == nullptr) last = &r; // all_runs is newest first
            }
            w.begin_object();
            w.field("id", s.id);
            w.field("name", s.name);
            w.field("runs", static_cast<std::int64_t>(runs));
            w.field("ok_runs", static_cast<std::int64_t>(ok));
            w.field("bytes_total", bytes_sum);
            w.field("avg_total_ms", runs == 0 ? 0.0 : total_ms_sum / static_cast<double>(runs));
            w.field("avg_fetch_ms", runs == 0 ? 0.0 : fetch_ms_sum / static_cast<double>(runs));
            w.key("last_run");
            if (last == nullptr) {
                w.null_value();
            } else {
                write_run(w, *last);
            }
            w.end_object();
        }
        w.end_array();
        w.end_object();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/model" && req.method == "GET") {
        const classify::Classifier& classifier = pipeline_.classifier();
        json::Writer w;
        w.begin_object();
        w.field("trained_at", classifier.trained_at());
        w.field("leave_one_out_accuracy", classifier.trained_accuracy());
        w.field("examples", static_cast<std::int64_t>(classifier.example_count()));
        w.field("vocabulary", static_cast<std::int64_t>(classifier.bayes().vocabulary_size()));
        w.field("kind", "multinomial naive Bayes, Laplace smoothing");
        w.key("classes");
        w.begin_array();
        for (const model::ClassSummary& c : classifier.bayes().summarize(10)) {
            w.begin_object();
            w.field("name", c.name);
            w.field("documents", c.documents);
            w.field("tokens", c.tokens);
            w.key("top_tokens");
            w.begin_array();
            for (const model::TokenWeight& t : c.top_tokens) {
                w.begin_object();
                w.field("token", t.token);
                w.field("count", t.count);
                w.field("lift", t.lift);
                w.end_object();
            }
            w.end_array();
            w.end_object();
        }
        w.end_array();
        w.end_object();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/schema" && req.method == "GET") {
        const auto it = req.query.find("source");
        if (it == req.query.end()) {
            respond_error(client_fd, 400, "source query parameter required");
            return;
        }
        const std::optional<store::Source> source = store_.find_source(it->second);
        if (!source.has_value()) {
            respond_error(client_fd, 404, "unknown source");
            return;
        }
        const std::string snapshot = store_.latest_records(it->second);
        const json::Value parsed = json::parse(snapshot);
        const store::SourceState state = store_.source_state(it->second);
        json::Writer w;
        w.begin_object();
        w.field("source_id", source->id);
        w.field("name", source->name);
        w.field("jurisdiction", source->jurisdiction);
        w.field("baseline_rate", state.baseline_rate);
        w.field("good_runs", state.good_runs);
        w.field_raw("snapshot", parsed.is_object() ? snapshot : "null");
        w.end_object();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/benchmark" && req.method == "POST") {
        std::size_t rounds = 5;
        const auto it = req.query.find("rounds");
        if (it != req.query.end()) {
            rounds = static_cast<std::size_t>(std::atoll(it->second.c_str()));
        }
        rounds = std::min<std::size_t>(std::max<std::size_t>(rounds, 1), 25);
        const std::vector<store::Source> sources = store_.sources();

        // A scale test is just the real pipeline, measured: every run below
        // fetches, parses, classifies, maps and resolves exactly as a
        // scheduled ingest would.
        const std::int64_t rss_before = metrics::current_rss_bytes();
        const double cpu_before = metrics::cpu_time_ms();
        std::int64_t runs = 0;
        std::int64_t ok = 0;
        std::int64_t records = 0;
        std::int64_t events_new = 0;
        std::int64_t bytes = 0;
        const Stopwatch watch;
        {
            const std::lock_guard<std::mutex> lock{run_mutex_};
            for (std::size_t round = 0; round < rounds; ++round) {
                for (const store::Source& s : sources) {
                    if (!s.enabled) continue;
                    const store::RunRecord r = pipeline_.run_source(s);
                    ++runs;
                    if (r.ok) ++ok;
                    records += r.records_extracted;
                    events_new += r.events_new;
                    bytes += r.bytes;
                }
            }
        }
        const double total_ms = watch.elapsed_ms();

        json::Writer w;
        w.begin_object();
        w.field("rounds", static_cast<std::int64_t>(rounds));
        w.field("runs", runs);
        w.field("ok_runs", ok);
        w.field("records_processed", records);
        w.field("events_new", events_new);
        w.field("bytes_processed", bytes);
        w.field("total_ms", total_ms);
        w.field("runs_per_sec", total_ms > 0.0 ? runs * 1000.0 / total_ms : 0.0);
        w.field("records_per_sec", total_ms > 0.0 ? records * 1000.0 / total_ms : 0.0);
        w.field("cpu_ms", metrics::cpu_time_ms() - cpu_before);
        w.field("rss_before_bytes", rss_before);
        w.field("rss_after_bytes", metrics::current_rss_bytes());
        w.end_object();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/sources" && req.method == "GET") {
        json::Writer w;
        w.begin_array();
        for (const store::Source& s : store_.sources()) {
            write_source(w, s, store_.source_state(s.id), store_.runs(1, s.id));
        }
        w.end_array();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/sources" && req.method == "POST") {
        try {
            const json::Value body = json::parse(req.body);
            const json::Value* name = body.find("name");
            const json::Value* url = body.find("url");
            const json::Value* jurisdiction = body.find("jurisdiction");
            if (name == nullptr || url == nullptr) {
                respond_error(client_fd, 400, "name and url are required");
                return;
            }
            const store::Source s = store_.add_source(
                name->as_string(), url->as_string(),
                jurisdiction == nullptr ? "" : jurisdiction->as_string());
            json::Writer w;
            write_source(w, s, store_.source_state(s.id), {});
            respond_json(client_fd, 200, w.str());
        } catch (const Error& e) {
            respond_error(client_fd, 400, e.what());
        }
        return;
    }

    if (req.path == "/api/run" && req.method == "POST") {
        const auto it = req.query.find("source");
        if (it == req.query.end() || it->second.empty()) {
            respond_error(client_fd, 400, "source query parameter required");
            return;
        }
        try {
            const std::lock_guard<std::mutex> lock{run_mutex_};
            const store::RunRecord run = pipeline_.run_source_id(it->second);
            respond_json(client_fd, 200, run.serialize());
        } catch (const Error& e) {
            respond_error(client_fd, 404, e.what());
        }
        return;
    }

    if (req.path == "/api/run_all" && req.method == "POST") {
        json::Writer w;
        w.begin_array();
        {
            const std::lock_guard<std::mutex> lock{run_mutex_};
            for (const store::Source& s : store_.sources()) {
                if (!s.enabled) continue;
                write_run(w, pipeline_.run_source(s));
            }
        }
        w.end_array();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/runs" && req.method == "GET") {
        std::size_t limit = 50;
        const auto limit_it = req.query.find("limit");
        if (limit_it != req.query.end()) {
            limit = static_cast<std::size_t>(std::atoll(limit_it->second.c_str()));
            limit = std::min<std::size_t>(std::max<std::size_t>(limit, 1), 1000);
        }
        const auto source_it = req.query.find("source");
        const std::string source = source_it == req.query.end() ? "" : source_it->second;
        json::Writer w;
        w.begin_array();
        for (const store::RunRecord& r : store_.runs(limit, source)) write_run(w, r);
        w.end_array();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/records" && req.method == "GET") {
        const auto it = req.query.find("source");
        if (it == req.query.end()) {
            respond_error(client_fd, 400, "source query parameter required");
            return;
        }
        const std::string snapshot = store_.latest_records(it->second);
        const json::Value parsed = json::parse(snapshot);
        const json::Value* records = parsed.find("records");
        respond_json(client_fd, 200, records == nullptr ? "[]" : records->serialize());
        return;
    }

    if (req.path == "/api/properties" && req.method == "GET") {
        json::Writer w;
        w.begin_array();
        for (const std::string& key : store_.property_keys()) {
            const std::vector<events::PropertyEvent> evs = store_.events_for(key);
            const events::Lifecycle life = events::reduce(evs);
            const events::PropertyEvent& latest = evs.back();
            w.begin_object();
            w.field("key", key);
            w.field("state", events::state_name(life.state));
            w.field("events", static_cast<std::int64_t>(evs.size()));
            auto detail = [&](const char* field) {
                for (auto it2 = evs.rbegin(); it2 != evs.rend(); ++it2) {
                    const auto found = it2->details.find(field);
                    if (found != it2->details.end()) return found->second;
                }
                return std::string{};
            };
            w.field("owner", detail("owner"));
            w.field("address", detail("address"));
            w.field("parcel_id", detail("parcel_id"));
            w.field("last_event_date", latest.event_date);
            std::vector<std::string> source_ids;
            for (const events::PropertyEvent& e : evs) {
                if (std::find(source_ids.begin(), source_ids.end(), e.source_id) ==
                    source_ids.end()) {
                    source_ids.push_back(e.source_id);
                }
            }
            w.field("sources", static_cast<std::int64_t>(source_ids.size()));
            w.end_object();
        }
        w.end_array();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/property" && req.method == "GET") {
        const auto it = req.query.find("key");
        if (it == req.query.end()) {
            respond_error(client_fd, 400, "key query parameter required");
            return;
        }
        const std::vector<events::PropertyEvent> evs = store_.events_for(it->second);
        if (evs.empty()) {
            respond_error(client_fd, 404, "unknown property");
            return;
        }
        const events::Lifecycle life = events::reduce(evs);
        json::Writer w;
        w.begin_object();
        w.field("key", it->second);
        w.field("state", events::state_name(life.state));
        w.key("transitions");
        w.begin_array();
        for (const events::Transition& t : life.transitions) {
            w.begin_object();
            w.field("state", events::state_name(t.state));
            w.field("event_id", t.event_id);
            w.field("event_date", t.event_date);
            w.end_object();
        }
        w.end_array();
        w.key("events");
        w.begin_array();
        for (const events::PropertyEvent& e : evs) w.raw_value(e.serialize());
        w.end_array();
        w.end_object();
        respond_json(client_fd, 200, w.str());
        return;
    }

    if (req.path == "/api/repairs" && req.method == "GET") {
        const auto it = req.query.find("source");
        const std::string source = it == req.query.end() ? "" : it->second;
        json::Writer w;
        w.begin_array();
        for (const store::RepairRecord& r : store_.repairs(source)) w.raw_value(r.serialize());
        w.end_array();
        respond_json(client_fd, 200, w.str());
        return;
    }

    respond_error(client_fd, 404, "no such endpoint: " + req.method + " " + req.path);
}

} // namespace dd::server
