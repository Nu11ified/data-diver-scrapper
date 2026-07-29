#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dd {
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what_arg) : std::runtime_error(what_arg) {}
};

namespace str {
std::string trim(std::string_view s);
std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);
std::string collapse_ws(std::string_view s);
std::vector<std::string> split(std::string_view s, char delim);

std::vector<std::string> tokenize_words(std::string_view s);

bool contains(std::string_view hay, std::string_view needle);
std::string replace_all(std::string_view s, std::string_view from, std::string_view to);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
bool is_digits(std::string_view s);
std::string strip_non_alnum(std::string_view s);
std::string slug(std::string_view s);

double jaro_winkler(std::string_view a, std::string_view b);

std::uint64_t hash64(std::string_view s);
std::string hex64(std::uint64_t v);
} // namespace str

namespace timeutil {
std::string iso_now();
std::int64_t unix_now();
std::string iso_from_unix(std::int64_t t);

std::optional<std::int64_t> epoch_seconds(std::string_view stamp);
double hours_between(std::string_view earlier, std::string_view later);
} // namespace timeutil

class Stopwatch {
public:
    Stopwatch() : start_{std::chrono::steady_clock::now()} {}
    double elapsed_ms() const;
    void reset();

private:
    std::chrono::steady_clock::time_point start_;
};

namespace fileio {
std::string read_file(const std::string& path);
void write_file_atomic(const std::string& path, std::string_view data);
void append_line(const std::string& path, std::string_view line);
bool exists(const std::string& path);
void ensure_dir(const std::string& path);
std::vector<std::string> read_lines(const std::string& path);
std::vector<std::string> list_dir(const std::string& path);
} // namespace fileio

namespace logging {
enum class Level { Debug, Info, Warn, Error };
void set_level(Level level);
void debug(std::string_view msg);
void info(std::string_view msg);
void warn(std::string_view msg);
void error(std::string_view msg);
} // namespace logging
} // namespace dd
