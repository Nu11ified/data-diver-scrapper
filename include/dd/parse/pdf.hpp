#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dd::pdf {
bool looks_like_pdf(std::string_view bytes);

std::vector<std::string> extract_text_lines(std::string_view bytes);
} // namespace dd::pdf
