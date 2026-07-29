#pragma once

#include "dd/ml/columns.hpp"
#include "dd/parse/document.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dd::schema {
enum class Kind { Id, Name, Address, Money, Date, Status, Text, Email, Phone, Number };

/// A composite cell splits into parts; a part index of -1 uses the whole cell.
/// Splitting is what lets one "36.9, -76.2" column fill latitude and longitude.
constexpr int kWholeValue = -1;
std::vector<std::string> split_parts(std::string_view value);

std::string_view kind_name(Kind k);
std::optional<Kind> kind_from_name(std::string_view name);

struct FieldDef {
    std::string name;
    Kind kind = Kind::Text;
    std::string role;
    bool identity = false;
    std::string authority;  // classifier label whose sources win conflicts on this field
    std::vector<std::string> synonyms;
    double min = 0.0;  // Number kind only; a closed range the value must fall in
    double max = 0.0;
    bool has_range = false;
};

class Registry {
public:
    static Registry load(const std::string& path);
    static Registry from_json(const std::string& text);

    const std::vector<FieldDef>& fields() const noexcept { return fields_; }
    const FieldDef* find(std::string_view name) const;

    std::vector<const FieldDef*> with_role(std::string_view role) const;

private:
    std::vector<FieldDef> fields_;
};

bool validate(const FieldDef& field, std::string_view value);
std::string normalize(const FieldDef& field, std::string_view value);
std::optional<double> parse_money(std::string_view value);
std::optional<std::string> parse_date(std::string_view value);

struct Coercion {
    bool ok = false;
    std::string value;       // normalized canonical form when ok
    bool reformatted = false;
};
Coercion coerce(const FieldDef& field, std::string_view raw);

struct FieldMapping {
    std::string field;              // canonical field name
    std::string source_label;
    double label_similarity = 0.0;  // lexicon similarity, [0,1]
    double value_pass_rate = 0.0;   // measured over sampled values, [0,1]
    double confidence = 0.0;        // combined; never a constant
    bool reformatted = false;       // values need extraction, not direct use
    int part = kWholeValue;         // which split part of the cell carries it
};

struct Mapping {
    std::vector<FieldMapping> fields;
    double confidence = 0.0;        // mean of accepted field confidences

    const FieldMapping* find(std::string_view field) const;
    std::string serialize() const;
    static Mapping deserialize(const std::string& text);
};

Mapping infer_mapping(const Registry& registry, const doc::Model& model,
                      const columns::ColumnModel* neural = nullptr);

double score_label(const FieldDef& field, const std::string& label);

struct Candidate {
    std::string field;
    std::string source_label;
    double label_similarity = 0.0;
    double value_pass_rate = 0.0;
    double confidence = 0.0;
    bool reformatted = false;
    double neural = 0.0;   // column transformer posterior for this pair
    bool accepted = false; // part of what infer_mapping would keep
    int part = kWholeValue;
};
std::vector<Candidate> score_candidates(const Registry& registry, const doc::Model& model,
                                        double floor,
                                        const columns::ColumnModel* neural = nullptr);

Mapping apply_overrides(const Registry& registry, const Mapping& mapping,
                        const std::map<std::string, std::string>& overrides,
                        const doc::Model& model);

struct CanonicalRecord {
    std::map<std::string, std::string> values;
    double completeness = 0.0; // accepted fields present / mapped fields
};

struct ExtractionResult {
    std::vector<CanonicalRecord> records;
    std::map<std::string, double> field_rates;
    double rate = 0.0;
};

ExtractionResult apply_mapping(const Registry& registry, const Mapping& mapping,
                               const doc::Model& model);
} // namespace dd::schema
