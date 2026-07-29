#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dd::render {
bool colors_enabled();

std::string paint(std::string_view style, std::string_view s);

void section(std::string_view title);

void kv(const std::vector<std::pair<std::string, std::string>>& pairs);

std::string meter(double value, int width = 12);

std::string stamp(std::string_view word);

void table(const std::vector<std::string>& header,
           const std::vector<std::vector<std::string>>& rows);
} // namespace dd::render
