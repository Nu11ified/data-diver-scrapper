#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dd {

// Thrown when a stage cannot do the job it advertises. The pipeline catches
// these per source and records the message on the run record, so one broken
// county cannot take down the process.
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

// Lowercase word tokens. Splits on non-alphanumeric runs and on camelCase
// boundaries, so "ownerName", "owner_name" and "Owner Name" all yield
// {"owner", "name"}.
std::vector<std::string> tokenize_words(std::string_view s);

bool contains(std::string_view hay, std::string_view needle);
std::string replace_all(std::string_view s, std::string_view from, std::string_view to);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
bool is_digits(std::string_view s);
std::string strip_non_alnum(std::string_view s);
std::string slug(std::string_view s);

// Similarity in [0,1]. Used for fuzzy label and name comparison.
double jaro_winkler(std::string_view a, std::string_view b);

std::uint64_t hash64(std::string_view s);
std::string hex64(std::uint64_t v);

} // namespace str

namespace timeutil {
std::string iso_now();
std::int64_t unix_now();
std::string iso_from_unix(std::int64_t t);

// Hours between two timestamps. Accepts a bare date ("2026-07-27"), which
// counts from midnight, as well as a full ISO stamp. Returns -1 when either
// side cannot be read as a date.
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
