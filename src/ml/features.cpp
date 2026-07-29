#include "dd/ml/features.hpp"

#include "dd/core/core.hpp"

#include <algorithm>
#include <array>

namespace dd::features {
namespace {
constexpr std::size_t kMaxBodyTokens = 600;

void add(Bag& bag, const std::string& token) { ++bag[token]; }

void add_words(Bag& bag, std::string_view text, std::string_view prefix,
               std::size_t limit = SIZE_MAX) {
    std::size_t used = 0;
    for (const std::string& word : str::tokenize_words(text)) {
        if (word.size() < 2 || is_stopword(word)) continue;
        if (str::is_digits(word)) continue; // numbers are values, not vocabulary
        add(bag, std::string{prefix} + word);
        if (++used >= limit) break;
    }
}
} // namespace

bool is_stopword(std::string_view word) {
    static constexpr std::array<std::string_view, 44> kStop = {
        "a",    "an",   "and",  "are",  "as",   "at",   "be",   "by",   "com",
        "for",  "from", "gov",  "has",  "have", "he",   "her",  "his",  "http",
        "https", "if",  "in",   "is",   "it",   "its",  "no",   "not",  "of",
        "on",   "or",   "our",  "she",  "that", "the",  "their", "this", "to",
        "was",  "we",   "were", "will", "with", "www",  "you",  "your"};
    return std::find(kStop.begin(), kStop.end(), word) != kStop.end();
}

Bag extract(const doc::Model& model, std::string_view url) {
    Bag bag;
    add(bag, "fmt:" + std::string{doc::format_name(model.format)});

    for (const std::string& label : model.labels) {
        for (const std::string& word : str::tokenize_words(label)) {
            if (word.size() < 2 || str::is_digits(word)) continue;
            add(bag, "label:" + word);
        }
    }
    add_words(bag, model.title, "title:");
    for (const std::string& heading : model.headings) add_words(bag, heading, "h:");
    add_words(bag, model.text, "", kMaxBodyTokens);
    if (!url.empty()) add_words(bag, url, "url:");
    return bag;
}
} // namespace dd::features
