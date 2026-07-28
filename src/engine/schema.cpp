#include "dd/engine/schema.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace dd::schema {
namespace {

struct FieldSpec {
    Field field;
    std::string_view name;
    bool identity;
    std::vector<std::string_view> synonyms;
};

const std::vector<FieldSpec>& specs() {
    static const std::vector<FieldSpec> kSpecs = {
        {Field::ParcelId, "parcel_id", true,
         {"parcel", "parcel number", "parcel id", "apn", "pin", "property id", "account number",
          "tax account", "account", "parcel no", "folio", "tax id", "instrument"}},
        {Field::Owner, "owner", true,
         {"owner", "owner name", "taxpayer", "taxpayer name", "property holder", "owner of record",
          "name", "defendant", "grantee", "grantor", "decedent", "applicant", "borrower"}},
        {Field::Address, "address", true,
         {"address", "property address", "situs address", "situs", "site address", "location",
          "street address", "property location"}},
        {Field::AmountDue, "amount_due", false,
         {"amount due", "amount owed", "delinquent amount", "balance", "balance due", "total due",
          "taxes due", "amount", "judgment amount", "fine", "fine assessed"}},
        {Field::AssessedValue, "assessed_value", false,
         {"assessed value", "assessment", "assessed total", "total assessed value", "market value",
          "appraised value", "land value", "estate value"}},
        {Field::SalePrice, "sale_price", false,
         {"sale price", "consideration", "opening bid", "minimum bid", "sold for", "price",
          "valuation", "declared valuation", "mortgage amount"}},
        {Field::EventDate, "event_date", false,
         {"date", "event date", "filing date", "filed", "recorded", "recording date", "issued",
          "issued date", "opened", "date of death", "lis pendens", "letters issued"}},
        {Field::AuctionDate, "auction_date", false,
         {"auction date", "sale date", "auction", "tax sale date", "redemption deadline",
          "compliance deadline", "compliance date", "deadline"}},
        {Field::CaseNumber, "case_number", false,
         {"case number", "case no", "case id", "docket", "docket number", "document number",
          "doc no", "permit number", "permit no", "sale number", "sale no", "sale id",
          "certificate number", "citation number", "file number"}},
        {Field::Status, "status", false,
         {"status", "case status", "state", "disposition", "stage", "exemption", "deed type",
          "estate type", "work class"}},
        {Field::Description, "description", false,
         {"description", "legal description", "violation", "violation description",
          "violation type", "scope of work", "description of work", "work", "notes", "remarks"}},
    };
    return kSpecs;
}

const FieldSpec& spec_for(Field f) {
    for (const FieldSpec& s : specs()) {
        if (s.field == f) return s;
    }
    throw Error("schema: unknown field");
}

// --------------------------------------------------------- value checks ----

bool has_alpha(std::string_view s) {
    return std::any_of(s.begin(), s.end(),
                       [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
}

std::size_t digit_count(std::string_view s) {
    return static_cast<std::size_t>(
        std::count_if(s.begin(), s.end(),
                      [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }));
}

const std::array<std::string_view, 12> kMonths = {"january", "february", "march",     "april",
                                                  "may",     "june",     "july",      "august",
                                                  "september", "october", "november", "december"};

int month_from_name(std::string_view word) {
    const std::string lowered = str::to_lower(word);
    for (std::size_t i = 0; i < kMonths.size(); ++i) {
        if (kMonths[i].substr(0, 3) == lowered.substr(0, std::min<std::size_t>(3, lowered.size())) &&
            (lowered.size() <= 3 || kMonths[i].substr(0, lowered.size()) == lowered)) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

std::string format_iso(int year, int month, int day) {
    if (year < 1800 || year > 2200 || month < 1 || month > 12 || day < 1 || day > 31) return {};
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
    return std::string{buffer};
}

// ------------------------------------------------------- label matching ----

// Similarity between a source label and one synonym: exact slug match is 1,
// otherwise combine token overlap with fuzzy whole-string similarity.
double label_synonym_similarity(const std::string& label_slug,
                                const std::vector<std::string>& label_tokens,
                                std::string_view synonym) {
    const std::string syn_slug = str::slug(synonym);
    if (label_slug == syn_slug) return 1.0;

    const std::vector<std::string> syn_tokens = str::tokenize_words(synonym);
    if (syn_tokens.empty() || label_tokens.empty()) return 0.0;

    double overlap = 0.0;
    for (const std::string& st : syn_tokens) {
        double best = 0.0;
        for (const std::string& lt : label_tokens) {
            best = std::max(best, str::jaro_winkler(st, lt));
        }
        overlap += best >= 0.92 ? best : 0.0;
    }
    // Divide by the longer side so "date" only half-matches "Sale Date":
    // a synonym that leaves label tokens unexplained is a weaker claim than
    // one that covers them all.
    const double token_score =
        overlap / static_cast<double>(std::max(syn_tokens.size(), label_tokens.size()));
    const double fuzzy = str::jaro_winkler(label_slug, syn_slug);
    return std::max(token_score, fuzzy >= 0.90 ? fuzzy : 0.0);
}

double label_similarity(const FieldSpec& spec, const std::string& label) {
    const std::string slug = str::slug(label);
    const std::vector<std::string> tokens = str::tokenize_words(label);
    double best = 0.0;
    for (std::string_view synonym : spec.synonyms) {
        best = std::max(best, label_synonym_similarity(slug, tokens, synonym));
        if (best >= 1.0) break;
    }
    return best;
}

constexpr double kAcceptThreshold = 0.60;
constexpr double kLabelWeight = 0.55;
constexpr double kValueWeight = 0.45;
constexpr std::size_t kSampleLimit = 25;

} // namespace

std::string_view field_name(Field f) { return spec_for(f).name; }

const std::vector<Field>& all_fields() {
    static const std::vector<Field> kAll = [] {
        std::vector<Field> out;
        for (const FieldSpec& s : specs()) out.push_back(s.field);
        return out;
    }();
    return kAll;
}

bool is_identity_field(Field f) { return spec_for(f).identity; }

std::optional<double> parse_money(std::string_view value) {
    std::string digits;
    bool negative = false;
    bool seen_digit = false;
    bool seen_dot = false;
    std::size_t currency = 0;
    for (char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            digits.push_back(c);
            seen_digit = true;
            continue;
        }
        if (c == '.') {
            if (seen_dot) return std::nullopt;
            seen_dot = true;
            digits.push_back(c);
            continue;
        }
        if (c == ',' || c == ' ') continue;
        if (c == '$') {
            ++currency;
            continue;
        }
        if (c == '(' || c == ')') continue; // accounting negatives
        if (c == '-' && !seen_digit) {
            negative = true;
            continue;
        }
        return std::nullopt;
    }
    if (!seen_digit || currency > 1) return std::nullopt;
    const double parsed = std::atof(digits.c_str());
    return negative ? -parsed : parsed;
}

std::optional<std::string> parse_date(std::string_view value) {
    const std::string cleaned = str::trim(value);
    if (cleaned.empty() || cleaned.size() > 40) return std::nullopt;

    // YYYY-MM-DD or YYYY/MM/DD
    int y = 0, m = 0, d = 0;
    if (std::sscanf(cleaned.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3 ||
        std::sscanf(cleaned.c_str(), "%4d/%2d/%2d", &y, &m, &d) == 3) {
        const std::string iso = format_iso(y, m, d);
        if (!iso.empty()) return iso;
    }
    // MM/DD/YYYY or MM-DD-YYYY
    if (std::sscanf(cleaned.c_str(), "%2d/%2d/%4d", &m, &d, &y) == 3 ||
        std::sscanf(cleaned.c_str(), "%2d-%2d-%4d", &m, &d, &y) == 3) {
        const std::string iso = format_iso(y, m, d);
        if (!iso.empty()) return iso;
    }
    // "June 15, 2026" / "15 June 2026"
    const std::vector<std::string> words = str::tokenize_words(cleaned);
    if (words.size() == 3) {
        if (const int month = month_from_name(words[0]); month != 0) {
            if (str::is_digits(words[1]) && str::is_digits(words[2])) {
                const std::string iso =
                    format_iso(std::atoi(words[2].c_str()), month, std::atoi(words[1].c_str()));
                if (!iso.empty()) return iso;
            }
        }
        if (const int month = month_from_name(words[1]); month != 0) {
            if (str::is_digits(words[0]) && str::is_digits(words[2])) {
                const std::string iso =
                    format_iso(std::atoi(words[2].c_str()), month, std::atoi(words[0].c_str()));
                if (!iso.empty()) return iso;
            }
        }
    }
    return std::nullopt;
}

bool validate(Field f, std::string_view raw) {
    const std::string value = str::trim(raw);
    if (value.empty()) return false;
    switch (f) {
    case Field::ParcelId: {
        if (value.size() < 3 || value.size() > 30) return false;
        if (parse_date(value).has_value()) return false;
        if (str::contains(value, "$")) return false;
        const std::size_t digits = digit_count(value);
        if (digits < 2) return false;
        return digits * 10 >= value.size() * 4; // at least 40% digits
    }
    case Field::Owner: {
        if (value.size() < 2 || value.size() > 80) return false;
        if (!has_alpha(value)) return false;
        if (str::contains(value, "$")) return false;
        return digit_count(value) * 2 <= value.size(); // names are mostly letters
    }
    case Field::Address: {
        if (value.size() < 5 || value.size() > 120) return false;
        if (!has_alpha(value)) return false;
        const std::string lowered = str::to_lower(value);
        if (str::contains(lowered, "po box")) return true;
        // A leading or embedded street number.
        return digit_count(value) >= 1 && digit_count(value) * 2 <= value.size();
    }
    case Field::AmountDue:
    case Field::AssessedValue:
    case Field::SalePrice: {
        const std::optional<double> money = parse_money(value);
        return money.has_value() && *money >= 0.0 && *money < 1e10;
    }
    case Field::EventDate:
    case Field::AuctionDate: return parse_date(value).has_value();
    case Field::CaseNumber: {
        if (value.size() < 3 || value.size() > 30) return false;
        if (str::contains(value, "$")) return false;
        if (parse_date(value).has_value()) return false;
        return digit_count(value) >= 2;
    }
    case Field::Status: return value.size() <= 60 && has_alpha(value);
    case Field::Description: return value.size() >= 3;
    }
    return false;
}

std::string normalize(Field f, std::string_view raw) {
    const std::string value = str::trim(raw);
    switch (f) {
    case Field::AmountDue:
    case Field::AssessedValue:
    case Field::SalePrice: {
        const std::optional<double> money = parse_money(value);
        if (!money.has_value()) return value;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", *money);
        return std::string{buffer};
    }
    case Field::EventDate:
    case Field::AuctionDate: {
        const std::optional<std::string> date = parse_date(value);
        return date.has_value() ? *date : value;
    }
    case Field::ParcelId: return str::to_upper(value);
    case Field::Owner:
    case Field::Address:
    case Field::CaseNumber:
    case Field::Status:
    case Field::Description: return str::collapse_ws(value);
    }
    return value;
}

const FieldMapping* Mapping::find(Field f) const {
    for (const FieldMapping& fm : fields) {
        if (fm.field == f) return &fm;
    }
    return nullptr;
}

std::string Mapping::serialize() const {
    json::Writer w;
    w.begin_object();
    w.field("confidence", confidence);
    w.key("fields");
    w.begin_array();
    for (const FieldMapping& fm : fields) {
        w.begin_object();
        w.field("field", field_name(fm.field));
        w.field("source_label", fm.source_label);
        w.field("label_similarity", fm.label_similarity);
        w.field("value_pass_rate", fm.value_pass_rate);
        w.field("confidence", fm.confidence);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.take();
}

Mapping Mapping::deserialize(const std::string& text) {
    const json::Value root = json::parse(text);
    Mapping out;
    const json::Value* confidence = root.find("confidence");
    if (confidence != nullptr) out.confidence = confidence->as_number();
    const json::Value* fields = root.find("fields");
    if (fields == nullptr || !fields->is_array()) throw Error("schema: mapping missing fields");
    for (const json::Value& entry : fields->items()) {
        FieldMapping fm;
        const json::Value* name = entry.find("field");
        if (name == nullptr) throw Error("schema: mapping field without name");
        bool found = false;
        for (Field f : all_fields()) {
            if (field_name(f) == name->as_string()) {
                fm.field = f;
                found = true;
                break;
            }
        }
        if (!found) continue; // a future schema version's field: ignore
        const json::Value* label = entry.find("source_label");
        if (label != nullptr) fm.source_label = label->as_string();
        const json::Value* sim = entry.find("label_similarity");
        if (sim != nullptr) fm.label_similarity = sim->as_number();
        const json::Value* rate = entry.find("value_pass_rate");
        if (rate != nullptr) fm.value_pass_rate = rate->as_number();
        const json::Value* conf = entry.find("confidence");
        if (conf != nullptr) fm.confidence = conf->as_number();
        out.fields.push_back(std::move(fm));
    }
    return out;
}

Mapping infer_mapping(const doc::Model& model) {
    struct Candidate {
        Field field;
        std::string label;
        double label_sim;
        double pass_rate;
        double combined;
    };
    std::vector<Candidate> candidates;

    for (const std::string& label : model.labels) {
        // Sample values under this label.
        std::vector<std::string> samples;
        for (const doc::RawRecord& record : model.records) {
            if (samples.size() >= kSampleLimit) break;
            const doc::Cell* cell = record.find(label);
            if (cell != nullptr && !cell->value.empty()) samples.push_back(cell->value);
        }
        for (const FieldSpec& spec : specs()) {
            const double sim = label_similarity(spec, label);
            double pass = 0.0;
            if (!samples.empty()) {
                std::size_t good = 0;
                for (const std::string& sample : samples) {
                    if (validate(spec.field, sample)) ++good;
                }
                pass = static_cast<double>(good) / static_cast<double>(samples.size());
            }
            const double combined = kLabelWeight * sim + kValueWeight * pass;
            if (combined >= kAcceptThreshold && pass > 0.0) {
                candidates.push_back(Candidate{spec.field, label, sim, pass, combined});
            }
        }
    }

    // Greedy assignment, best score first; each field and each label used at
    // most once. Stable so equal scores resolve deterministically.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.combined > b.combined; });
    Mapping mapping;
    std::vector<std::string> used_labels;
    for (const Candidate& c : candidates) {
        if (mapping.find(c.field) != nullptr) continue;
        if (std::find(used_labels.begin(), used_labels.end(), c.label) != used_labels.end()) {
            continue;
        }
        mapping.fields.push_back(
            FieldMapping{c.field, c.label, c.label_sim, c.pass_rate, c.combined});
        used_labels.push_back(c.label);
    }
    std::sort(mapping.fields.begin(), mapping.fields.end(),
              [](const FieldMapping& a, const FieldMapping& b) {
                  return static_cast<int>(a.field) < static_cast<int>(b.field);
              });

    const bool has_identity =
        std::any_of(mapping.fields.begin(), mapping.fields.end(),
                    [](const FieldMapping& fm) { return is_identity_field(fm.field); });
    if (!has_identity) {
        // Without an identity field records cannot be resolved to properties;
        // report that as an unusable mapping rather than a half-working one.
        mapping.fields.clear();
        mapping.confidence = 0.0;
        return mapping;
    }

    double total = 0.0;
    for (const FieldMapping& fm : mapping.fields) total += fm.confidence;
    mapping.confidence =
        mapping.fields.empty() ? 0.0 : total / static_cast<double>(mapping.fields.size());
    return mapping;
}

ExtractionResult apply_mapping(const Mapping& mapping, const doc::Model& model) {
    ExtractionResult out;
    if (mapping.fields.empty() || model.records.empty()) return out;

    std::map<std::string, std::size_t> field_hits;
    for (const doc::RawRecord& record : model.records) {
        CanonicalRecord canonical;
        std::size_t valid = 0;
        for (const FieldMapping& fm : mapping.fields) {
            const doc::Cell* cell = record.find(fm.source_label);
            if (cell == nullptr) continue;
            if (!validate(fm.field, cell->value)) continue;
            canonical.values[std::string{field_name(fm.field)}] = normalize(fm.field, cell->value);
            ++field_hits[std::string{field_name(fm.field)}];
            ++valid;
        }
        canonical.completeness =
            static_cast<double>(valid) / static_cast<double>(mapping.fields.size());
        out.records.push_back(std::move(canonical));
    }

    const double n = static_cast<double>(model.records.size());
    for (const FieldMapping& fm : mapping.fields) {
        const std::string name{field_name(fm.field)};
        out.field_rates[name] = static_cast<double>(field_hits[name]) / n;
    }
    double total = 0.0;
    for (const CanonicalRecord& r : out.records) total += r.completeness;
    out.rate = out.records.empty() ? 0.0 : total / static_cast<double>(out.records.size());
    return out;
}

} // namespace dd::schema
