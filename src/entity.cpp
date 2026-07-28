#include "dd/entity.hpp"

#include "dd/core.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace dd::entity {
namespace {

// Street suffix and directional spellings collapse to one form so
// "1402 North Main Street" and "1402 N MAIN ST" produce the same key.
const std::vector<std::pair<std::string_view, std::string_view>>& abbreviations() {
    static const std::vector<std::pair<std::string_view, std::string_view>> kAbbrev = {
        {"street", "st"},   {"avenue", "ave"},   {"road", "rd"},      {"lane", "ln"},
        {"drive", "dr"},    {"court", "ct"},     {"place", "pl"},     {"boulevard", "blvd"},
        {"parkway", "pkwy"}, {"highway", "hwy"}, {"circle", "cir"},   {"terrace", "ter"},
        {"trail", "trl"},   {"square", "sq"},    {"north", "n"},      {"south", "s"},
        {"east", "e"},      {"west", "w"},       {"apartment", "apt"}, {"suite", "ste"},
        {"unit", "unit"},   {"number", "no"},
    };
    return kAbbrev;
}

std::string canonical_word(const std::string& word) {
    for (const auto& [full, abbrev] : abbreviations()) {
        if (word == full || word == abbrev) return std::string{abbrev};
    }
    return word;
}

} // namespace

std::string normalize_parcel(std::string_view parcel) {
    return str::to_upper(str::strip_non_alnum(parcel));
}

std::string normalize_address(std::string_view address) {
    std::vector<std::string> out;
    for (const std::string& word : str::tokenize_words(address)) {
        out.push_back(canonical_word(word));
    }
    return str::join(out, " ");
}

std::string property_key(std::string_view jurisdiction, std::string_view parcel,
                         std::string_view address) {
    const std::string scope = str::slug(jurisdiction);
    const std::string p = normalize_parcel(parcel);
    if (!p.empty()) return scope + "|p:" + p;
    const std::string a = normalize_address(address);
    if (!a.empty()) return scope + "|a:" + a;
    return {};
}

bool same_owner(std::string_view a, std::string_view b) {
    std::vector<std::string> ta = str::tokenize_words(a);
    std::vector<std::string> tb = str::tokenize_words(b);
    if (ta.empty() || tb.empty()) return false;
    std::sort(ta.begin(), ta.end());
    std::sort(tb.begin(), tb.end());
    if (ta == tb) return true;
    // Fuzzy fallback for typos: compare the sorted-token strings.
    return str::jaro_winkler(str::join(ta, " "), str::join(tb, " ")) >= 0.93;
}

} // namespace dd::entity
