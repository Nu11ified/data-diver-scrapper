#pragma once

#include "dd/parse/document.hpp"

#include <map>
#include <string>
#include <string_view>

namespace dd::features {
using Bag = std::map<std::string, int>;

Bag extract(const doc::Model& model, std::string_view url);

bool is_stopword(std::string_view word);
} // namespace dd::features
