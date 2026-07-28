#include "render.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace dd::render {
namespace {

std::string code_for(std::string_view style) {
    if (style == "dim") return "\033[2m";
    if (style == "bold") return "\033[1m";
    if (style == "teal") return "\033[36m";
    if (style == "red") return "\033[31m";
    if (style == "amber") return "\033[33m";
    if (style == "green") return "\033[32m";
    return "";
}

// Display width of a string ignoring ANSI escapes. The meter glyphs are
// multi-byte UTF-8 but single-column; count bytes outside escapes and
// outside UTF-8 continuation bytes.
std::size_t visible_width(std::string_view s) {
    std::size_t width = 0;
    bool in_escape = false;
    for (unsigned char c : s) {
        if (in_escape) {
            if (c == 'm') in_escape = false;
            continue;
        }
        if (c == '\033') {
            in_escape = true;
            continue;
        }
        if ((c & 0xC0) == 0x80) continue; // UTF-8 continuation byte
        ++width;
    }
    return width;
}

} // namespace

bool colors_enabled() {
    static const bool enabled = [] {
        if (std::getenv("NO_COLOR") != nullptr) return false;
        return isatty(STDOUT_FILENO) == 1;
    }();
    return enabled;
}

std::string paint(std::string_view style, std::string_view s) {
    if (!colors_enabled()) return std::string{s};
    const std::string code = code_for(style);
    if (code.empty()) return std::string{s};
    return code + std::string{s} + "\033[0m";
}

void section(std::string_view title) {
    std::string line = "== " + std::string{title} + " ";
    while (visible_width(line) < 60) line.push_back('=');
    std::printf("\n%s\n", paint("bold", line).c_str());
}

void kv(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::size_t widest = 0;
    for (const auto& [key, value] : pairs) widest = std::max(widest, key.size());
    for (const auto& [key, value] : pairs) {
        std::string padded = key;
        padded.resize(widest, ' ');
        std::printf("  %s  %s\n", paint("dim", padded).c_str(), value.c_str());
    }
}

std::string meter(double value, int width) {
    value = std::max(0.0, std::min(1.0, value));
    const int filled = static_cast<int>(value * width + 0.5);
    std::string bar;
    for (int i = 0; i < width; ++i) bar += i < filled ? "█" : "░";
    char pct[8];
    std::snprintf(pct, sizeof(pct), "%3.0f%%", value * 100.0);
    const char* tone = value >= 0.8 ? "teal" : value >= 0.5 ? "amber" : "red";
    return paint(tone, bar) + " " + pct;
}

std::string stamp(std::string_view word) {
    std::string upper;
    for (char c : word) upper.push_back(static_cast<char>(std::toupper(c)));
    const char* tone = "dim";
    if (word == "ok" || word == "healed" || word == "aligned") tone = "teal";
    else if (word == "review" || word == "unchanged" || word == "pending") tone = "amber";
    else if (word == "failed" || word == "drift") tone = "red";
    return paint(tone, "[" + upper + "]");
}

void table(const std::vector<std::string>& header,
           const std::vector<std::vector<std::string>>& rows) {
    const std::size_t columns = header.size();
    std::vector<std::size_t> widths(columns, 0);
    for (std::size_t c = 0; c < columns; ++c) widths[c] = visible_width(header[c]);
    for (const std::vector<std::string>& row : rows) {
        for (std::size_t c = 0; c < columns && c < row.size(); ++c) {
            widths[c] = std::max(widths[c], visible_width(row[c]));
        }
    }

    auto print_row = [&](const std::vector<std::string>& cells, bool dim) {
        std::string line = "  ";
        for (std::size_t c = 0; c < columns; ++c) {
            const std::string cell = c < cells.size() ? cells[c] : "";
            line += dim ? paint("dim", cell) : cell;
            const std::size_t pad = widths[c] - std::min(widths[c], visible_width(cell));
            line.append(pad, ' ');
            if (c + 1 < columns) line += "  ";
        }
        std::printf("%s\n", line.c_str());
    };

    print_row(header, true);
    std::string rule = "  ";
    for (std::size_t c = 0; c < columns; ++c) {
        rule.append(widths[c], '-');
        if (c + 1 < columns) rule += "  ";
    }
    std::printf("%s\n", paint("dim", rule).c_str());
    for (const std::vector<std::string>& row : rows) print_row(row, false);
}

} // namespace dd::render
