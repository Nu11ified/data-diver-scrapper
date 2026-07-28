#include "dd/engine/schema.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace dd::schema {
namespace {

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

    // A one-word synonym is weak evidence: it only matches a label that IS
    // that word. Partial credit here is how "name" wrongly claimed
    // "street_name" for the owner field.
    if (syn_tokens.size() == 1 && label_tokens.size() > 1) return 0.0;

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

constexpr double kAcceptThreshold = 0.65;
constexpr double kLabelWeight = 0.55;
constexpr double kValueWeight = 0.45;
constexpr std::size_t kSampleLimit = 25;

// Money, date and id validators are decisive: values that pass are strong
// evidence in themselves. Text-family kinds validate almost anything, so for
// them the label must carry the case on its own. This is what stops a
// street_name column from becoming the owner because both end in "name".
bool validator_is_weak(Kind k) {
    return k == Kind::Name || k == Kind::Address || k == Kind::Status || k == Kind::Text;
}
constexpr double kWeakValidatorLabelFloor = 0.70;

// Pass statistics of sampled values under `label` for `field`, measured on
// the actual document the mapping will run against. Coercion counts as a
// pass; `reformatted` reports when most passing values needed extraction.
struct PassStats {
    double rate = 0.0;
    bool reformatted = false;
};

PassStats measured_pass(const FieldDef& field, const std::string& label,
                        const doc::Model& model) {
    std::size_t sampled = 0;
    std::size_t good = 0;
    std::size_t extracted = 0;
    for (const doc::RawRecord& record : model.records) {
        if (sampled >= kSampleLimit) break;
        const doc::Cell* cell = record.find(label);
        if (cell == nullptr || cell->value.empty()) continue;
        ++sampled;
        const Coercion c = coerce(field, cell->value);
        if (c.ok) {
            ++good;
            if (c.reformatted) ++extracted;
        }
    }
    PassStats out;
    if (sampled == 0) return out;
    out.rate = static_cast<double>(good) / static_cast<double>(sampled);
    out.reformatted = extracted * 2 > good;
    return out;
}

// Registry order decides the display order of mapping fields.
std::size_t field_order(const Registry& registry, std::string_view name) {
    const std::vector<FieldDef>& fields = registry.fields();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name) return i;
    }
    return fields.size();
}

void sort_by_schema_order(const Registry& registry, std::vector<FieldMapping>& fields) {
    std::stable_sort(fields.begin(), fields.end(),
                     [&](const FieldMapping& a, const FieldMapping& b) {
                         return field_order(registry, a.field) < field_order(registry, b.field);
                     });
}

double mean_confidence(const std::vector<FieldMapping>& fields) {
    if (fields.empty()) return 0.0;
    double total = 0.0;
    for (const FieldMapping& fm : fields) total += fm.confidence;
    return total / static_cast<double>(fields.size());
}

} // namespace

// -------------------------------------------------------------- kinds ------

std::string_view kind_name(Kind k) {
    switch (k) {
    case Kind::Id: return "id";
    case Kind::Name: return "name";
    case Kind::Address: return "address";
    case Kind::Money: return "money";
    case Kind::Date: return "date";
    case Kind::Status: return "status";
    case Kind::Text: return "text";
    }
    return "text";
}

std::optional<Kind> kind_from_name(std::string_view name) {
    for (Kind k : {Kind::Id, Kind::Name, Kind::Address, Kind::Money, Kind::Date, Kind::Status,
                   Kind::Text}) {
        if (kind_name(k) == name) return k;
    }
    return std::nullopt;
}

// ------------------------------------------------------------ registry -----

Registry Registry::from_json(const std::string& text) {
    const json::Value root = json::parse(text);
    const json::Value* fields = root.find("fields");
    if (fields == nullptr || !fields->is_array()) {
        throw Error("schema: file needs a 'fields' array");
    }
    Registry out;
    for (const json::Value& entry : fields->items()) {
        FieldDef def;
        const json::Value* name = entry.find("name");
        if (name == nullptr || name->as_string().empty()) {
            throw Error("schema: every field needs a name");
        }
        def.name = str::slug(name->as_string());
        if (out.find(def.name) != nullptr) {
            throw Error("schema: duplicate field name '" + def.name + "'");
        }
        const json::Value* kind = entry.find("kind");
        if (kind != nullptr) {
            const std::optional<Kind> parsed = kind_from_name(kind->as_string());
            if (!parsed.has_value()) {
                throw Error("schema: unknown kind '" + kind->as_string() + "' on field '" +
                            def.name + "'");
            }
            def.kind = *parsed;
        }
        const json::Value* role = entry.find("role");
        if (role != nullptr) def.role = role->as_string();
        const json::Value* identity = entry.find("identity");
        if (identity != nullptr) def.identity = identity->as_bool();
        const json::Value* synonyms = entry.find("synonyms");
        if (synonyms != nullptr && synonyms->is_array()) {
            for (const json::Value& s : synonyms->items()) def.synonyms.push_back(s.as_string());
        }
        // The field's own name always matches itself.
        def.synonyms.push_back(str::join(str::tokenize_words(def.name), " "));
        out.fields_.push_back(std::move(def));
    }
    if (out.fields_.empty()) throw Error("schema: no fields defined");
    const bool has_identity = std::any_of(out.fields_.begin(), out.fields_.end(),
                                          [](const FieldDef& f) { return f.identity; });
    if (!has_identity) {
        throw Error("schema: at least one field must be marked identity so records can "
                    "resolve to properties");
    }
    return out;
}

Registry Registry::load(const std::string& path) {
    return from_json(fileio::read_file(path));
}

const FieldDef* Registry::find(std::string_view name) const {
    for (const FieldDef& f : fields_) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

std::vector<const FieldDef*> Registry::with_role(std::string_view role) const {
    std::vector<const FieldDef*> out;
    for (const FieldDef& f : fields_) {
        if (f.role == role) out.push_back(&f);
    }
    return out;
}

// ---------------------------------------------------------- validators -----

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

bool validate(const FieldDef& field, std::string_view raw) {
    const std::string value = str::trim(raw);
    if (value.empty()) return false;
    switch (field.kind) {
    case Kind::Id: {
        if (value.size() < 3 || value.size() > 30) return false;
        if (parse_date(value).has_value()) return false;
        if (str::contains(value, "$")) return false;
        const std::size_t digits = digit_count(value);
        if (digits < 2) return false;
        return digits * 10 >= value.size() * 4; // at least 40% digits
    }
    case Kind::Name: {
        if (value.size() < 2 || value.size() > 80) return false;
        if (!has_alpha(value)) return false;
        if (str::contains(value, "$")) return false;
        return digit_count(value) * 2 <= value.size(); // names are mostly letters
    }
    case Kind::Address: {
        if (value.size() < 5 || value.size() > 120) return false;
        if (!has_alpha(value)) return false;
        const std::string lowered = str::to_lower(value);
        if (str::contains(lowered, "po box")) return true;
        // A leading or embedded street number.
        return digit_count(value) >= 1 && digit_count(value) * 2 <= value.size();
    }
    case Kind::Money: {
        const std::optional<double> money = parse_money(value);
        return money.has_value() && *money >= 0.0 && *money < 1e10;
    }
    case Kind::Date: return parse_date(value).has_value();
    case Kind::Status: return value.size() <= 60 && has_alpha(value);
    case Kind::Text: return value.size() >= 3;
    }
    return false;
}

std::string normalize(const FieldDef& field, std::string_view raw) {
    const std::string value = str::trim(raw);
    switch (field.kind) {
    case Kind::Money: {
        const std::optional<double> money = parse_money(value);
        if (!money.has_value()) return value;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", *money);
        return std::string{buffer};
    }
    case Kind::Date: {
        const std::optional<std::string> date = parse_date(value);
        return date.has_value() ? *date : value;
    }
    case Kind::Id: return str::to_upper(str::collapse_ws(value));
    case Kind::Name:
    case Kind::Address:
    case Kind::Status:
    case Kind::Text: return str::collapse_ws(value);
    }
    return value;
}

namespace {

// Candidate substrings inside a composite value: whitespace tokens with edge
// punctuation stripped, plus the remainder after a label-style colon.
std::vector<std::string> embedded_candidates(std::string_view raw) {
    std::vector<std::string> out;
    const std::string value = str::trim(raw);
    const std::size_t colon = value.find(':');
    if (colon != std::string::npos && colon + 1 < value.size()) {
        out.push_back(str::trim(value.substr(colon + 1)));
    }
    for (const std::string& token : str::split(value, ' ')) {
        std::string t = token;
        while (!t.empty() && std::strchr(":;,#()[]", t.front()) != nullptr) t.erase(0, 1);
        while (!t.empty() && std::strchr(":;,#()[]", t.back()) != nullptr) t.pop_back();
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

bool kind_coercible(Kind k) { return k == Kind::Id || k == Kind::Money || k == Kind::Date; }

} // namespace

Coercion coerce(const FieldDef& field, std::string_view raw) {
    if (validate(field, raw)) {
        return Coercion{true, normalize(field, raw), false};
    }
    if (!kind_coercible(field.kind)) return {};
    for (const std::string& candidate : embedded_candidates(raw)) {
        if (validate(field, candidate)) {
            return Coercion{true, normalize(field, candidate), true};
        }
    }
    return {};
}

// ------------------------------------------------------------- mapping -----

double score_label(const FieldDef& field, const std::string& label) {
    const std::string slug = str::slug(label);
    const std::vector<std::string> tokens = str::tokenize_words(label);
    double best = 0.0;
    for (const std::string& synonym : field.synonyms) {
        best = std::max(best, label_synonym_similarity(slug, tokens, synonym));
        if (best >= 1.0) break;
    }
    return best;
}

const FieldMapping* Mapping::find(std::string_view field) const {
    for (const FieldMapping& fm : fields) {
        if (fm.field == field) return &fm;
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
        w.field("field", fm.field);
        w.field("source_label", fm.source_label);
        w.field("label_similarity", fm.label_similarity);
        w.field("value_pass_rate", fm.value_pass_rate);
        w.field("confidence", fm.confidence);
        w.field("reformatted", fm.reformatted);
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
        if (name == nullptr || name->as_string().empty()) {
            throw Error("schema: mapping field without name");
        }
        fm.field = name->as_string();
        const json::Value* label = entry.find("source_label");
        if (label != nullptr) fm.source_label = label->as_string();
        const json::Value* sim = entry.find("label_similarity");
        if (sim != nullptr) fm.label_similarity = sim->as_number();
        const json::Value* rate = entry.find("value_pass_rate");
        if (rate != nullptr) fm.value_pass_rate = rate->as_number();
        const json::Value* conf = entry.find("confidence");
        if (conf != nullptr) fm.confidence = conf->as_number();
        const json::Value* ref = entry.find("reformatted");
        if (ref != nullptr) fm.reformatted = ref->as_bool();
        out.fields.push_back(std::move(fm));
    }
    return out;
}

std::vector<Candidate> score_candidates(const Registry& registry, const doc::Model& model,
                                        double floor, const columns::ColumnModel* neural) {
    std::vector<Candidate> candidates;
    for (const std::string& label : model.labels) {
        // Name evidence comes from two independent sources: the lexicon and,
        // when a trained column model is on hand, its posterior over fields
        // for this column's name and sample values. The stronger one counts.
        std::map<std::string, double> posterior;
        if (neural != nullptr && neural->trained()) {
            std::vector<std::string> samples;
            for (const doc::RawRecord& record : model.records) {
                if (samples.size() >= 3) break;
                const doc::Cell* cell = record.find(label);
                if (cell != nullptr && !cell->value.empty()) samples.push_back(cell->value);
            }
            posterior = neural->predict(label, samples).distribution;
        }
        for (const FieldDef& field : registry.fields()) {
            const double sim = score_label(field, label);
            const auto nn_it = posterior.find(field.name);
            const double nn = nn_it == posterior.end() ? 0.0 : nn_it->second;
            const double name_evidence = std::max(sim, nn);
            const PassStats pass = measured_pass(field, label, model);
            const double combined = kLabelWeight * name_evidence + kValueWeight * pass.rate;
            if (pass.rate <= 0.0 || combined < floor) continue;
            Candidate c;
            c.field = field.name;
            c.source_label = label;
            c.label_similarity = sim;
            c.neural = nn;
            c.value_pass_rate = pass.rate;
            c.confidence = combined;
            c.reformatted = pass.reformatted;
            candidates.push_back(std::move(c));
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         return a.confidence > b.confidence;
                     });

    // Mark what automatic assignment keeps: the auto rules (threshold plus
    // the weak-validator label floor), greedy, one field per label.
    std::vector<std::string> used_fields;
    std::vector<std::string> used_labels;
    for (Candidate& c : candidates) {
        if (c.confidence < kAcceptThreshold) continue;
        const FieldDef* field = registry.find(c.field);
        if (field == nullptr) continue;
        // Text validators pass anything, so only lexicon evidence can clear
        // the weak-validator floor; the transformer's posterior counts as
        // name evidence only where values can veto it.
        if (validator_is_weak(field->kind) && c.label_similarity < kWeakValidatorLabelFloor) {
            continue;
        }
        if (std::find(used_fields.begin(), used_fields.end(), c.field) != used_fields.end()) {
            continue;
        }
        if (std::find(used_labels.begin(), used_labels.end(), c.source_label) !=
            used_labels.end()) {
            continue;
        }
        c.accepted = true;
        used_fields.push_back(c.field);
        used_labels.push_back(c.source_label);
    }
    return candidates;
}

Mapping infer_mapping(const Registry& registry, const doc::Model& model,
                      const columns::ColumnModel* neural) {
    Mapping mapping;
    for (const Candidate& c : score_candidates(registry, model, kAcceptThreshold, neural)) {
        if (!c.accepted) continue;
        mapping.fields.push_back(FieldMapping{c.field, c.source_label, c.label_similarity,
                                              c.value_pass_rate, c.confidence, c.reformatted});
    }
    sort_by_schema_order(registry, mapping.fields);

    const bool has_identity =
        std::any_of(mapping.fields.begin(), mapping.fields.end(), [&](const FieldMapping& fm) {
            const FieldDef* def = registry.find(fm.field);
            return def != nullptr && def->identity;
        });
    if (!has_identity) {
        mapping.fields.clear();
        mapping.confidence = 0.0;
        return mapping;
    }

    mapping.confidence = mean_confidence(mapping.fields);
    return mapping;
}

Mapping apply_overrides(const Registry& registry, const Mapping& mapping,
                        const std::map<std::string, std::string>& overrides,
                        const doc::Model& model) {
    Mapping out = mapping;
    for (const auto& [name, label] : overrides) {
        const FieldDef* field = registry.find(name);
        if (field == nullptr) continue; // not a canonical field in this schema
        std::erase_if(out.fields, [&](const FieldMapping& fm) { return fm.field == name; });
        if (label.empty()) continue; // force-unmap
        if (std::find(model.labels.begin(), model.labels.end(), label) == model.labels.end()) {
            continue; // pinned label absent from this document: stay unmapped
        }
        // The override owns its label: no other field may keep it.
        std::erase_if(out.fields,
                      [&](const FieldMapping& fm) { return fm.source_label == label; });
        const PassStats pass = measured_pass(*field, label, model);
        FieldMapping fm;
        fm.field = name;
        fm.source_label = label;
        fm.label_similarity = score_label(*field, label);
        fm.value_pass_rate = pass.rate;
        fm.confidence = kLabelWeight * fm.label_similarity + kValueWeight * fm.value_pass_rate;
        fm.reformatted = pass.reformatted;
        out.fields.push_back(std::move(fm));
    }
    sort_by_schema_order(registry, out.fields);
    out.confidence = mean_confidence(out.fields);
    return out;
}

ExtractionResult apply_mapping(const Registry& registry, const Mapping& mapping,
                               const doc::Model& model) {
    ExtractionResult out;
    if (mapping.fields.empty() || model.records.empty()) return out;

    std::map<std::string, std::size_t> field_hits;
    std::size_t known_fields = 0;
    for (const FieldMapping& fm : mapping.fields) {
        if (registry.find(fm.field) != nullptr) ++known_fields;
    }
    if (known_fields == 0) return out;

    for (const doc::RawRecord& record : model.records) {
        CanonicalRecord canonical;
        std::size_t valid = 0;
        for (const FieldMapping& fm : mapping.fields) {
            const FieldDef* field = registry.find(fm.field);
            if (field == nullptr) continue; // mapping from another schema version
            const doc::Cell* cell = record.find(fm.source_label);
            if (cell == nullptr) continue;
            const Coercion coerced = coerce(*field, cell->value);
            if (!coerced.ok) continue;
            canonical.values[fm.field] = coerced.value;
            ++field_hits[fm.field];
            ++valid;
        }
        canonical.completeness = static_cast<double>(valid) / static_cast<double>(known_fields);
        out.records.push_back(std::move(canonical));
    }

    const double n = static_cast<double>(model.records.size());
    for (const FieldMapping& fm : mapping.fields) {
        if (registry.find(fm.field) == nullptr) continue;
        out.field_rates[fm.field] = static_cast<double>(field_hits[fm.field]) / n;
    }
    double total = 0.0;
    for (const CanonicalRecord& r : out.records) total += r.completeness;
    out.rate = out.records.empty() ? 0.0 : total / static_cast<double>(out.records.size());
    return out;
}

} // namespace dd::schema
