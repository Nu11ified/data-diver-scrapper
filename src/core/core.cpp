#include "dd/core/core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace dd {
namespace {
bool is_alnum(unsigned char c) { return std::isalnum(c) != 0; }
} // namespace

namespace str {
std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return std::string{s.substr(b, e - b)};
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string to_upper(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

std::string collapse_ws(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            in_space = true;
            continue;
        }
        if (in_space && !out.empty()) out.push_back(' ');
        in_space = false;
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(delim, start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(s.substr(start));
            break;
        }
        parts.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::vector<std::string> tokenize_words(std::string_view s) {
    std::vector<std::string> words;
    std::string current;
    char previous = '\0';
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!is_alnum(uc)) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            previous = '\0';
            continue;
        }
        const bool camel_boundary = std::islower(static_cast<unsigned char>(previous)) != 0 &&
                                    std::isupper(uc) != 0;
        if (camel_boundary && !current.empty()) {
            words.push_back(current);
            current.clear();
        }
        current.push_back(static_cast<char>(std::tolower(uc)));
        previous = c;
    }
    if (!current.empty()) words.push_back(current);
    return words;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string{s};
    std::string out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(from, start);
        if (pos == std::string_view::npos) {
            out.append(s.substr(start));
            return out;
        }
        out.append(s.substr(start, pos - start));
        out.append(to);
        start = pos + from.size();
    }
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

bool is_digits(std::string_view s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string strip_non_alnum(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (is_alnum(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

std::string slug(std::string_view s) {
    const std::vector<std::string> words = tokenize_words(s);
    return join(words, "_");
}

double jaro_winkler(std::string_view a, std::string_view b) {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;

    const std::size_t window = std::max<std::size_t>(std::max(a.size(), b.size()) / 2, 1) - 1;
    std::vector<bool> a_used(a.size(), false);
    std::vector<bool> b_used(b.size(), false);

    std::size_t matches = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::size_t lo = i > window ? i - window : 0;
        const std::size_t hi = std::min(i + window + 1, b.size());
        for (std::size_t j = lo; j < hi; ++j) {
            if (b_used[j] || a[i] != b[j]) continue;
            a_used[i] = true;
            b_used[j] = true;
            ++matches;
            break;
        }
    }
    if (matches == 0) return 0.0;

    std::size_t transpositions = 0;
    std::size_t k = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!a_used[i]) continue;
        while (k < b.size() && !b_used[k]) ++k;
        if (k < b.size() && a[i] != b[k]) ++transpositions;
        ++k;
    }

    const double m = static_cast<double>(matches);
    const double jaro = (m / static_cast<double>(a.size()) + m / static_cast<double>(b.size()) +
                         (m - static_cast<double>(transpositions) / 2.0) / m) /
                        3.0;

    std::size_t prefix = 0;
    const std::size_t max_prefix = std::min<std::size_t>(4, std::min(a.size(), b.size()));
    while (prefix < max_prefix && a[prefix] == b[prefix]) ++prefix;

    return jaro + static_cast<double>(prefix) * 0.1 * (1.0 - jaro);
}

std::uint64_t hash64(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex64(std::uint64_t v) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << v;
    return out.str();
}
} // namespace str

namespace timeutil {
std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string iso_from_unix(std::int64_t t) {
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string{buffer};
}

std::string iso_now() { return iso_from_unix(unix_now()); }

std::optional<std::int64_t> epoch_seconds(std::string_view stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    const std::string text{stamp};
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) < 3) {
        if (std::sscanf(text.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3) return std::nullopt;
    }
    if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return static_cast<std::int64_t>(timegm(&tm));
}

double hours_between(std::string_view earlier, std::string_view later) {
    const std::optional<std::int64_t> a = epoch_seconds(earlier);
    const std::optional<std::int64_t> b = epoch_seconds(later);
    if (!a.has_value() || !b.has_value()) return -1.0;
    return static_cast<double>(*b - *a) / 3600.0;
}
} // namespace timeutil

double Stopwatch::elapsed_ms() const {
    const auto delta = std::chrono::steady_clock::now() - start_;
    return std::chrono::duration<double, std::milli>(delta).count();
}

void Stopwatch::reset() { start_ = std::chrono::steady_clock::now(); }

namespace fileio {
std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open file: " + path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void ensure_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && !std::filesystem::is_directory(path)) {
        throw Error("cannot create directory " + path + ": " + ec.message());
    }
}

void write_file_atomic(const std::string& path, std::string_view data) {
    const std::filesystem::path target{path};
    if (target.has_parent_path()) ensure_dir(target.parent_path().string());
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot write file: " + temp);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) throw Error("short write: " + temp);
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) throw Error("cannot rename " + temp + ": " + ec.message());
}

void append_line(const std::string& path, std::string_view line) {
    const std::filesystem::path target{path};
    if (target.has_parent_path()) ensure_dir(target.parent_path().string());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw Error("cannot append to file: " + path);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.put('\n');
    if (!out) throw Error("short append: " + path);
}

bool exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path, std::ios::binary);
    if (!in) return lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> entries;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        entries.push_back(entry.path().string());
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}
} // namespace fileio

namespace logging {
namespace {
Level g_level = Level::Info;
std::mutex g_mutex;

void emit(Level level, std::string_view tag, std::string_view msg) {
    if (level < g_level) return;
    const std::lock_guard<std::mutex> lock{g_mutex};
    std::cerr << timeutil::iso_now() << " [" << tag << "] " << msg << '\n';
}
} // namespace

void set_level(Level level) { g_level = level; }
void debug(std::string_view msg) { emit(Level::Debug, "debug", msg); }
void info(std::string_view msg) { emit(Level::Info, "info", msg); }
void warn(std::string_view msg) { emit(Level::Warn, "warn", msg); }
void error(std::string_view msg) { emit(Level::Error, "error", msg); }
} // namespace logging
} // namespace dd
