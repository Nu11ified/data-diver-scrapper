#include "dd/engine/heal.hpp"

#include <algorithm>

namespace dd::heal {
namespace {
constexpr double kCollapseRatio = 0.6;

constexpr double kRecoveryRatio = 0.7;

constexpr double kAutoAcceptConfidence = 0.75;

std::string describe(const schema::Mapping& mapping, std::string_view field) {
    const schema::FieldMapping* fm = mapping.find(field);
    return fm == nullptr ? std::string{"(unmapped)"} : "'" + fm->source_label + "'";
}
} // namespace

double auto_accept_threshold() { return kAutoAcceptConfidence; }

Assessment assess(const store::SourceState& state, const doc::Model& model,
                  const schema::ExtractionResult& with_stored_mapping) {
    Assessment out;
    out.baseline_rate = state.baseline_rate;
    out.current_rate = with_stored_mapping.rate;
    if (!state.has_mapping || state.good_runs == 0) return out; // nothing learned yet

    out.fingerprint_changed =
        !state.fingerprint.empty() && state.fingerprint != model.structure_fingerprint();

    const bool no_records = model.records.empty();
    const bool collapsed = out.current_rate < state.baseline_rate * kCollapseRatio;

    if (no_records) {
        out.drift = true;
        out.reason = "no records extracted where baseline expected " +
                     std::to_string(state.baseline_rate);
        return out;
    }
    if (collapsed) {
        out.drift = true;
        out.reason = "extraction rate " + std::to_string(out.current_rate) +
                     " collapsed below baseline " + std::to_string(state.baseline_rate);
        if (out.fingerprint_changed) out.reason += " after a structure change";
        return out;
    }
    return out;
}

Proposal propose(const schema::Registry& registry, const doc::Model& model,
                 const schema::Mapping& previous, double baseline_rate,
                 const columns::ColumnModel* neural) {
    Proposal out;
    out.candidate = schema::infer_mapping(registry, model, neural);
    out.result = schema::apply_mapping(registry, out.candidate, model);

    if (out.candidate.fields.empty()) {
        out.confidence = 0.0;
        out.acceptable = false;
        out.changes.push_back("no usable mapping found in the new structure");
        return out;
    }

    const double recovery =
        baseline_rate > 0.0 ? std::min(1.0, out.result.rate / baseline_rate) : out.result.rate;
    out.confidence = 0.5 * out.candidate.confidence + 0.5 * recovery;

    const bool recovered_enough =
        baseline_rate > 0.0 ? out.result.rate >= baseline_rate * kRecoveryRatio
                            : out.result.rate > 0.0;
    out.acceptable = recovered_enough && out.confidence >= kAutoAcceptConfidence;

    for (const schema::FieldDef& field : registry.fields()) {
        const schema::FieldMapping* before = previous.find(field.name);
        const schema::FieldMapping* after = out.candidate.find(field.name);
        if (before == nullptr && after == nullptr) continue;
        const std::string b = describe(previous, field.name);
        const std::string a = describe(out.candidate, field.name);
        if (b == a) continue;
        out.changes.push_back(field.name + ": " + b + " -> " + a);
    }
    if (out.changes.empty()) out.changes.push_back("mapping unchanged; values revalidated");
    return out;
}
} // namespace dd::heal
