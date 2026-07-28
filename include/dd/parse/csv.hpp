#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dd::csv {

struct Table {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
};

// RFC 4180 style parsing with quoted fields, embedded delimiters, embedded
// newlines and doubled quotes. Delimiter is auto detected between comma,
// semicolon, tab and pipe by counting occurrences on the first line.
Table parse(std::string_view text);
Table parse_with_delimiter(std::string_view text, char delimiter);
char detect_delimiter(std::string_view text);

// True when the first row reads as column names rather than data: mostly
// non-numeric cells that are distinct from the shape of the following rows.
bool first_row_is_header(const std::vector<std::vector<std::string>>& rows);

} // namespace dd::csv
