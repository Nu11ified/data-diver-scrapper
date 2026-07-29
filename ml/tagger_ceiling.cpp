// What any column tagger could be worth on the golden key. Replays the accept
// logic of schema::score_candidates with the tagger's verdict replaced by an
// oracle that names the answer-key column with confidence 1.0 and abstains
// everywhere else, then rescores the key. Mode "real" reproduces the shipped
// path and must match infer_mapping exactly, which is what makes the oracle
// number readable.
//
// Build:
//   c++ -std=c++20 -O2 -I include ml/tagger_ceiling.cpp build-ml/libdd_engine.a \
//       -lcurl -lz -o /tmp/tagger_ceiling && ./tagger_ceiling real|oracle|none

#include "dd/core/core.hpp"
#include "dd/engine/bench.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"
#include "dd/ml/columns.hpp"
#include "dd/parse/document.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace dd;

namespace {

constexpr double kAccept = 0.65;
constexpr double kPartAccept = 0.80;
constexpr double kWeakLabelFloor = 0.70;
constexpr double kLabelWeight = 0.55;
constexpr double kValueWeight = 0.45;
constexpr std::size_t kSampleLimit = 25;
constexpr int kWhole = schema::kWholeValue;

bool weak(schema::Kind k) {
    return k == schema::Kind::Name || k == schema::Kind::Address || k == schema::Kind::Status ||
           k == schema::Kind::Text;
}

std::string part_value(const std::string& raw, int part) {
    if (part == kWhole) return str::trim(raw);
    const std::vector<std::string> parts = schema::split_parts(raw);
    if (part < 0 || static_cast<std::size_t>(part) >= parts.size()) return {};
    return parts[static_cast<std::size_t>(part)];
}

double pass_rate(const schema::FieldDef& field, const std::string& label, const doc::Model& model,
                 int part) {
    std::size_t sampled = 0, good = 0;
    for (const doc::RawRecord& record : model.records) {
        if (sampled >= kSampleLimit) break;
        const doc::Cell* cell = record.find(label);
        if (cell == nullptr || cell->value.empty()) continue;
        const std::string piece = part_value(cell->value, part);
        if (piece.empty()) continue;
        ++sampled;
        if (schema::coerce(field, piece).ok) ++good;
    }
    return sampled == 0 ? 0.0 : static_cast<double>(good) / static_cast<double>(sampled);
}

std::size_t stable_parts(const std::string& label, const doc::Model& model) {
    std::size_t agreed = 0, sampled = 0;
    for (const doc::RawRecord& record : model.records) {
        if (sampled >= kSampleLimit) break;
        const doc::Cell* cell = record.find(label);
        if (cell == nullptr || cell->value.empty()) continue;
        const std::size_t parts = schema::split_parts(cell->value).size();
        ++sampled;
        if (sampled == 1) agreed = parts;
        else if (parts != agreed) return 0;
    }
    if (sampled < 2 || agreed < 2 || agreed > 6) return 0;
    return agreed;
}

struct Cand {
    std::string field, label;
    int part = kWhole;
    double sim = 0.0, nn = 0.0, rate = 0.0, conf = 0.0;
};

std::map<std::string, std::string> replay(const schema::Registry& registry,
                                          const doc::Model& model,
                                          const std::map<std::string, std::map<std::string, double>>& verdict) {
    std::vector<Cand> cands;
    for (const std::string& label : model.labels) {
        const auto vit = verdict.find(label);
        const std::size_t parts = stable_parts(label, model);
        const std::vector<std::string> tokens = str::tokenize_words(label);
        for (const schema::FieldDef& field : registry.fields()) {
            double nn = 0.0;
            if (vit != verdict.end()) {
                const auto fit = vit->second.find(field.name);
                if (fit != vit->second.end()) nn = fit->second;
            }
            const double sim = schema::score_label(field, label);
            const double evidence = std::max(sim, nn);
            const double rate = pass_rate(field, label, model, kWhole);
            const double conf = kLabelWeight * evidence + kValueWeight * rate;
            if (rate > 0.0 && conf >= kAccept) cands.push_back({field.name, label, kWhole, sim, nn, rate, conf});
            if (tokens.size() != parts) continue;
            for (std::size_t p = 0; p < parts; ++p) {
                const double prate = pass_rate(field, label, model, static_cast<int>(p));
                if (prate <= 0.0) continue;
                const double pev = schema::score_label(field, tokens[p]);
                const double pconf = kLabelWeight * pev + kValueWeight * prate;
                if (pconf < kAccept) continue;
                cands.push_back({field.name, label, static_cast<int>(p), sim, nn, prate, pconf});
            }
        }
    }
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Cand& a, const Cand& b) { return a.conf > b.conf; });

    std::map<std::string, std::string> accepted;
    std::set<std::string> used_fields, used_slots;
    for (const Cand& c : cands) {
        const bool is_part = c.part != kWhole;
        if (c.conf < (is_part ? kPartAccept : kAccept)) continue;
        const schema::FieldDef* field = registry.find(c.field);
        if (field == nullptr) continue;
        if (weak(field->kind) && (is_part || c.sim < kWeakLabelFloor)) continue;
        if (used_fields.count(c.field) != 0) continue;
        const std::string slot = is_part ? c.label + "#" + std::to_string(c.part) : c.label;
        if (used_slots.count(slot) != 0) continue;
        if (is_part && used_slots.count(c.label) != 0) continue;
        if (!is_part) {
            bool part_taken = false;
            for (const std::string& used : used_slots) {
                if (used.rfind(c.label + "#", 0) == 0) part_taken = true;
            }
            if (part_taken) continue;
        }
        accepted[c.field] = c.label;
        used_fields.insert(c.field);
        used_slots.insert(slot);
    }
    const bool has_identity = std::any_of(accepted.begin(), accepted.end(), [&](const auto& kv) {
        const schema::FieldDef* def = registry.find(kv.first);
        return def != nullptr && def->identity;
    });
    if (!has_identity) accepted.clear();
    return accepted;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "real";
    const schema::Registry registry = schema::Registry::load("data/schema.json");
    const std::vector<bench::Golden> golden = bench::load_golden("data/golden/golden.json");
    store::Store store{"var"};
    store.seed("data/sources.json");
    std::optional<columns::ColumnModel> neural = columns::ColumnModel::load("data/model/column_model.json");

    bench::MappingScore total;
    std::size_t mismatch = 0;
    for (const bench::Golden& g : golden) {
        const std::optional<store::CachedFetch> cached = store.fetch_cache(g.source_id);
        if (!cached.has_value()) continue;
        const doc::Model model = doc::build_auto(cached->content_type, cached->body);

        std::map<std::string, std::map<std::string, double>> verdict;
        for (const std::string& label : model.labels) {
            if (mode == "real") {
                std::vector<std::string> samples;
                for (const doc::RawRecord& record : model.records) {
                    if (samples.size() >= 3) break;
                    const doc::Cell* cell = record.find(label);
                    if (cell != nullptr && !cell->value.empty()) samples.push_back(cell->value);
                }
                const columns::Prediction p = neural->predict(label, samples);
                if (p.confidence >= 0.7 && p.label != "none") verdict[label][p.label] = p.confidence;
            } else if (mode == "oracle") {
                for (const auto& [field, labels] : g.fields) {
                    if (std::find(labels.begin(), labels.end(), label) != labels.end()) {
                        verdict[label][field] = 1.0;
                    }
                }
            } else if (mode == "oracle1") {
                // One verdict per column, which is all a real tagger can emit:
                // a field the key marks required wins over an optional one.
                std::string best;
                bool best_required = false;
                for (const auto& [field, labels] : g.fields) {
                    if (std::find(labels.begin(), labels.end(), label) == labels.end()) continue;
                    const bool required =
                        std::find(labels.begin(), labels.end(), "") == labels.end();
                    if (best.empty() || (required && !best_required)) {
                        best = field;
                        best_required = required;
                    }
                }
                if (!best.empty()) verdict[label][best] = 1.0;
            }
        }
        const std::map<std::string, std::string> accepted = replay(registry, model, verdict);
        const bench::MappingScore s = bench::score_mapping(g, registry, accepted);
        total.tp += s.tp;
        total.spurious += s.spurious;
        total.missing += s.missing;

        if (mode == "real") {
            std::map<std::string, std::string> truth;
            for (const schema::FieldMapping& fm :
                 schema::infer_mapping(registry, model, &*neural).fields) {
                truth[fm.field] = fm.source_label;
            }
            if (truth != accepted) {
                ++mismatch;
                std::printf("  replay mismatch on %s\n", g.source_id.c_str());
            }
        }
        std::printf("  %-30s %zu/%zu/%zu\n", g.source_id.c_str(), s.tp, s.spurious, s.missing);
        if (argc > 2 && std::string{argv[2]} == "dump") {
            for (const auto& [field, label] : accepted) {
                std::printf("      %-18s <- %s\n", field.c_str(), label.c_str());
            }
        }
    }
    std::printf("mode %s: tp %zu spurious %zu missing %zu -> P %.1f%% R %.1f%% F1 %.1f%%\n",
                mode.c_str(), total.tp, total.spurious, total.missing, total.precision() * 100.0,
                total.recall() * 100.0, total.f1() * 100.0);
    if (mode == "real") {
        std::printf("replay mismatches against infer_mapping: %zu\n", mismatch);
    }
    return 0;
}
