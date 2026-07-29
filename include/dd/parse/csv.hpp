#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dd::csv {
struct Table {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
};

Table parse(std::string_view text);
Table parse_with_delimiter(std::string_view text, char delimiter);
char detect_delimiter(std::string_view text);

bool first_row_is_header(const std::vector<std::vector<std::string>>& rows);
} // namespace dd::csv
