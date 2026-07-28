#pragma once

#include "dd/ml/columns.hpp"
#include "dd/parse/document.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dd::schema {

// Validator families. A field's kind decides how its values are checked and
// normalized; the fields themselves are configuration, not code.
enum class Kind { Id, Name, Address, Money, Date, Status, Text };

std::string_view kind_name(Kind k);
std::optional<Kind> kind_from_name(std::string_view name);

// One canonical field, defined in the schema file. The role ties a field to
// engine behaviour without hardcoding its name:
//   parcel / address    property identity (parcel preferred)
//   owner               display + event details
//   status              feeds event kind refinement (e.g. "sold at auction")
//   event_date          the event's date
//   fallback_date       used when no event_date mapped (e.g. auction date)
//   amount              the event amount; first mapped amount field wins,
//                       in schema order
// A field may have no role: it is extracted and carried, nothing more.
struct FieldDef {
    std::string name;
    Kind kind = Kind::Text;
    std::string role;
    bool identity = false;
    std::vector<std::string> synonyms;
};

// The set of labels we need to fill, loaded from JSON. Immutable once built.
class Registry {
public:
    // Throws dd::Error on unreadable or malformed schema files, on duplicate
    // field names, and on a schema with no identity field.
    static Registry load(const std::string& path);
    static Registry from_json(const std::string& text);

    const std::vector<FieldDef>& fields() const noexcept { return fields_; }
    const FieldDef* find(std::string_view name) const;

    // Fields carrying a role, in schema order.
    std::vector<const FieldDef*> with_role(std::string_view role) const;

private:
    std::vector<FieldDef> fields_;
};

// Validators. Each checks whether a raw source value is plausible for the
// field's kind and yields the normalized form (money as plain decimal, dates
// as ISO 8601). Mapping confidence is built from measured pass rates.
bool validate(const FieldDef& field, std::string_view value);
std::string normalize(const FieldDef& field, std::string_view value);
std::optional<double> parse_money(std::string_view value);
std::optional<std::string> parse_date(std::string_view value);

// Smart reformatting. When a raw value fails its kind validator outright,
// decisive kinds (id, money, date) scan the value for an embedded token that
// does validate: "Account: 123-456" yields a parcel, "Filed 07/28/2026 by
// clerk" yields a date, "$1,204.77 past due" yields an amount. The result is
// still validator-approved, never guessed; `reformatted` records that the
// value needed extraction. Weak kinds never coerce.
struct Coercion {
    bool ok = false;
    std::string value;       // normalized canonical form when ok
    bool reformatted = false;
};
Coercion coerce(const FieldDef& field, std::string_view raw);

// How one source dialect maps onto one canonical field.
struct FieldMapping {
    std::string field;              // canonical field name
    std::string source_label;
    double label_similarity = 0.0;  // lexicon similarity, [0,1]
    double value_pass_rate = 0.0;   // measured over sampled values, [0,1]
    double confidence = 0.0;        // combined; never a constant
    bool reformatted = false;       // values need extraction, not direct use
};

struct Mapping {
    std::vector<FieldMapping> fields;
    double confidence = 0.0;        // mean of accepted field confidences

    const FieldMapping* find(std::string_view field) const;
    std::string serialize() const;
    static Mapping deserialize(const std::string& text);
};

// Learns a mapping from the labels and sample values of a document. Labels
// are scored against each field's synonyms with fuzzy token matching, values
// against the kind validator; a field is accepted when the combined score
// clears the threshold. Each source label maps to at most one field.
Mapping infer_mapping(const Registry& registry, const doc::Model& model,
                      const columns::ColumnModel* neural = nullptr);

// Lexicon similarity of one (field, label) pair, in [0,1]: the same scorer
// infer_mapping uses.
double score_label(const FieldDef& field, const std::string& label);

// Every (field, label) pairing whose combined evidence clears `floor`,
// including the ones infer_mapping rejects. This is what an operator reviews:
// near-misses come with their measured scores and the caller shows sample
// values so a human can confirm or refuse.
struct Candidate {
    std::string field;
    std::string source_label;
    double label_similarity = 0.0;
    double value_pass_rate = 0.0;
    double confidence = 0.0;
    bool reformatted = false;
    double neural = 0.0;   // column transformer posterior for this pair
    bool accepted = false; // part of what infer_mapping would keep
};
std::vector<Candidate> score_candidates(const Registry& registry, const doc::Model& model,
                                        double floor,
                                        const columns::ColumnModel* neural = nullptr);

// Applies operator overrides (canonical field name -> source label; empty
// label = force-unmap) on top of an inferred or healed mapping. An
// overridden field maps to its label when the document carries it, with the
// label similarity from the lexicon and the value pass rate measured on this
// document; when the pinned label is absent the field stays unmapped rather
// than falling back to inference.
Mapping apply_overrides(const Registry& registry, const Mapping& mapping,
                        const std::map<std::string, std::string>& overrides,
                        const doc::Model& model);

// One record after mapping: canonical field -> normalized value.
struct CanonicalRecord {
    std::map<std::string, std::string> values;
    double completeness = 0.0; // accepted fields present / mapped fields
};

struct ExtractionResult {
    std::vector<CanonicalRecord> records;
    // Fraction of records where the field mapped, was present and validated.
    std::map<std::string, double> field_rates;
    // Overall extraction rate: mean of per-record completeness. This is the
    // number drift detection watches.
    double rate = 0.0;
};

ExtractionResult apply_mapping(const Registry& registry, const Mapping& mapping,
                               const doc::Model& model);

} // namespace dd::schema
