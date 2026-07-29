#include "dd/engine/entity.hpp"

#include "dd/core/core.hpp"

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
        {"av", "ave"},      {"crescent", "cres"},
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
        std::string canonical = canonical_word(word);
        // Treasurer exports zero-pad house numbers ("0555 Liberty St"); the
        // assessor does not. Same number, same key.
        if (canonical.size() > 1 && str::is_digits(canonical)) {
            const std::size_t first = canonical.find_first_not_of('0');
            canonical = first == std::string::npos ? "0" : canonical.substr(first);
        }
        out.push_back(std::move(canonical));
    }
    return str::join(out, " ");
}

namespace {

bool is_directional(const std::string& word) {
    return word == "n" || word == "s" || word == "e" || word == "w" || word == "ne" ||
           word == "nw" || word == "se" || word == "sw";
}

bool is_suffix(const std::string& word) {
    static const std::vector<std::string> kSuffixes = {
        "st", "ave", "rd", "ln", "dr", "ct", "pl", "blvd", "pkwy", "hwy", "cir", "ter",
        "trl", "sq", "way", "cres", "run", "loop", "pt", "walk"};
    return std::find(kSuffixes.begin(), kSuffixes.end(), word) != kSuffixes.end();
}

bool is_unit_marker(const std::string& word) {
    return word == "apt" || word == "ste" || word == "unit" || word == "no" || word == "#";
}

} // namespace

Address parse_address(std::string_view address) {
    Address out;
    const std::vector<std::string> words = str::tokenize_words(normalize_address(address));
    if (words.empty()) return out;
    // "3200 BLOCK OF ARGONNE AVENUE" names a stretch of street, not a building.
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        if ((words[i] == "block" || words[i] == "blk") && words[i + 1] == "of") return out;
    }

    std::size_t at = 0;
    if (str::is_digits(words[0]) || (words[0].size() > 1 && std::isdigit(
                                         static_cast<unsigned char>(words[0][0])) != 0)) {
        out.number = words[0];
        at = 1;
    }
    std::vector<std::string> street;
    for (; at < words.size(); ++at) {
        const std::string& word = words[at];
        if (is_unit_marker(word) && at + 1 < words.size()) {
            out.unit = words[at + 1];
            ++at;
            continue;
        }
        if (is_directional(word)) {
            if (out.directional.empty()) out.directional = word;
            continue;
        }
        if (is_suffix(word)) {
            out.suffix = word;
            continue;
        }
        street.push_back(word);
    }
    // A trailing lone letter after a suffix is a unit ("3 COMMERCIAL PL A").
    if (out.unit.empty() && !street.empty() && street.back().size() == 1 &&
        std::isalpha(static_cast<unsigned char>(street.back()[0])) != 0 && street.size() > 1) {
        out.unit = street.back();
        street.pop_back();
    }
    out.street = str::join(street, " ");
    out.locatable = !out.number.empty() && out.number != "0" && !out.street.empty();
    return out;
}

std::string address_join_key(const Address& address) {
    if (!address.locatable) return {};
    return address.number + " " + address.street + (address.unit.empty() ? "" : " #" + address.unit);
}

bool compatible(const Address& a, const Address& b) {
    if (!a.directional.empty() && !b.directional.empty() && a.directional != b.directional) {
        return false;
    }
    if (!a.suffix.empty() && !b.suffix.empty() && a.suffix != b.suffix) return false;
    return true;
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
