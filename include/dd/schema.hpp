#pragma once

#include "dd/document.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dd::schema {

// The canonical property-event schema. Every source dialect compiles onto
// these fields.
enum class Field {
    ParcelId,
    Owner,
    Address,
    AmountDue,
    AssessedValue,
    SalePrice,
    EventDate,
    AuctionDate,
    CaseNumber,
    Status,
    Description,
};

std::string_view field_name(Field f);
const std::vector<Field>& all_fields();

// Fields that identify the property. A mapping that finds none of these
// cannot resolve records and is not accepted.
bool is_identity_field(Field f);

// Validators. Each checks whether a raw source value is plausible for the
// field and yields the normalized form (money as plain decimal, dates as ISO
// 8601). Mapping confidence is built from measured validator pass rates.
bool validate(Field f, std::string_view value);
std::string normalize(Field f, std::string_view value);
std::optional<double> parse_money(std::string_view value);
std::optional<std::string> parse_date(std::string_view value);

// How one source dialect maps onto one canonical field.
struct FieldMapping {
    Field field = Field::Description;
    std::string source_label;
    double label_similarity = 0.0;  // lexicon similarity, [0,1]
    double value_pass_rate = 0.0;   // measured over sampled values, [0,1]
    double confidence = 0.0;        // combined; never a constant
};

struct Mapping {
    std::vector<FieldMapping> fields;
    double confidence = 0.0;        // mean of accepted field confidences

    const FieldMapping* find(Field f) const;
    std::string serialize() const;
    static Mapping deserialize(const std::string& text);
};

// Learns a mapping from the labels and sample values of a document. Labels
// are scored against a synonym lexicon with fuzzy token matching, values
// against the field validators; a field is accepted when the combined score
// clears the threshold. Each source label maps to at most one field.
Mapping infer_mapping(const doc::Model& model);

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

ExtractionResult apply_mapping(const Mapping& mapping, const doc::Model& model);

} // namespace dd::schema
