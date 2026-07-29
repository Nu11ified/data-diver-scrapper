#include "dd/parse/csv.hpp"

#include "dd/core/core.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace dd::csv {
namespace {
bool looks_numeric(std::string_view s) {
    const std::string cleaned = str::trim(s);
    if (cleaned.empty()) return false;
    std::size_t digits = 0;
    for (char c : cleaned) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            ++digits;
            continue;
        }
        if (c == '.' || c == ',' || c == '-' || c == '+' || c == '$' || c == '%' || c == ' ' ||
            c == '/') {
            continue;
        }
        return false;
    }
    return digits > 0;
}
} // namespace

char detect_delimiter(std::string_view text) {
    const std::size_t eol = text.find('\n');
    const std::string_view first = text.substr(0, eol == std::string_view::npos ? text.size() : eol);
    constexpr std::array<char, 4> kCandidates = {',', ';', '\t', '|'};
    char best = ',';
    std::size_t best_count = 0;
    for (char candidate : kCandidates) {
        std::size_t count = 0;
        bool quoted = false;
        for (char c : first) {
            if (c == '"') quoted = !quoted;
            else if (c == candidate && !quoted) ++count;
        }
        if (count > best_count) {
            best_count = count;
            best = candidate;
        }
    }
    return best;
}

Table parse_with_delimiter(std::string_view text, char delimiter) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false;
    bool cell_started = false;

    auto end_cell = [&] {
        row.push_back(cell);
        cell.clear();
        cell_started = false;
    };
    auto end_row = [&] {
        end_cell();
        const bool empty_row = row.size() == 1 && str::trim(row[0]).empty();
        if (!empty_row) rows.push_back(row);
        row.clear();
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell.push_back(c);
            }
            continue;
        }
        if (c == '"' && !cell_started) {
            quoted = true;
            cell_started = true;
            continue;
        }
        if (c == delimiter) {
            end_cell();
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            end_row();
            continue;
        }
        cell.push_back(c);
        cell_started = true;
    }
    if (!cell.empty() || !row.empty()) end_row();

    Table table;
    if (rows.empty()) return table;
    if (first_row_is_header(rows)) {
        for (const std::string& name : rows.front()) table.header.push_back(str::trim(name));
        table.rows.assign(rows.begin() + 1, rows.end());
    } else {
        table.rows = std::move(rows);
    }
    return table;
}

Table parse(std::string_view text) { return parse_with_delimiter(text, detect_delimiter(text)); }

bool first_row_is_header(const std::vector<std::vector<std::string>>& rows) {
    if (rows.empty()) return false;
    const std::vector<std::string>& first = rows.front();
    if (first.empty()) return false;

    std::size_t numeric_in_first = 0;
    std::size_t empty_in_first = 0;
    for (const std::string& cell : first) {
        if (looks_numeric(cell)) ++numeric_in_first;
        if (str::trim(cell).empty()) ++empty_in_first;
    }
    if (empty_in_first > 0) return false;
    if (numeric_in_first > 0) return false;
    if (rows.size() == 1) return true;

    for (std::size_t col = 0; col < first.size(); ++col) {
        std::size_t numeric_below = 0;
        std::size_t seen = 0;
        for (std::size_t r = 1; r < rows.size(); ++r) {
            if (col >= rows[r].size()) continue;
            ++seen;
            if (looks_numeric(rows[r][col])) ++numeric_below;
        }
        if (seen > 0 && numeric_below * 2 > seen) return true;
    }

    const bool all_short = std::all_of(first.begin(), first.end(), [](const std::string& cell) {
        return str::trim(cell).size() <= 40;
    });
    return all_short;
}
} // namespace dd::csv
