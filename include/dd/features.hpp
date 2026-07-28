#pragma once

#include "dd/document.hpp"

#include <map>
#include <string>
#include <string_view>

namespace dd::features {

// Token -> count. Deterministic order matters for stable serialization.
using Bag = std::map<std::string, int>;

// Turns a document into classifier features: field labels, title and heading
// words, format, and body text terms. Labels and headings carry a prefix so
// "owner" in a column name and "owner" in prose stay distinct signals.
// The url, when given, contributes path tokens; training passes "" so the
// model never learns from corpus file names.
Bag extract(const doc::Model& model, std::string_view url);

bool is_stopword(std::string_view word);

} // namespace dd::features
