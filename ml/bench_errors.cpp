// Decomposes the golden-key mapping error: which field/column pair produced
// each spurious and missing mapping, and whether the column tagger was the
// evidence that carried it. Reads only; changes nothing.
//
// Build (no CMake target, this is analysis, not shipped code):
//   c++ -std=c++20 -O2 -I include ml/bench_errors.cpp build-ml/libdd_engine.a \
//       -lcurl -lz -o /tmp/bench_errors && ./bench_errors

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"
#include "dd/engine/bench.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"
#include "dd/ml/columns.hpp"
#include "dd/parse/document.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace dd;

namespace {

std::vector<std::string> samples_of(const doc::Model& model, const std::string& label,
                                    std::size_t n) {
    std::vector<std::string> out;
    for (const doc::RawRecord& record : model.records) {
        if (out.size() >= n) break;
        const doc::Cell* cell = record.find(label);
        if (cell != nullptr && !cell->value.empty()) out.push_back(cell->value);
    }
    return out;
}

std::map<std::string, std::string> as_field_map(const schema::Mapping& m) {
    std::map<std::string, std::string> out;
    for (const schema::FieldMapping& fm : m.fields) out[fm.field] = fm.source_label;
    return out;
}

const schema::FieldMapping* find_field(const schema::Mapping& m, const std::string& field) {
    for (const schema::FieldMapping& fm : m.fields) {
        if (fm.field == field) return &fm;
    }
    return nullptr;
}

const schema::Candidate* find_candidate(const std::vector<schema::Candidate>& cands,
                                        const std::string& field, const std::string& label,
                                        int part) {
    for (const schema::Candidate& c : cands) {
        if (c.field == field && c.source_label == label && c.part == part) return &c;
    }
    return nullptr;
}

void emit_string(json::Writer& w, const std::string& key, const std::string& value) {
    w.field(key, value);
}

} // namespace

int main(int argc, char** argv) {
    const std::string state_dir = argc > 1 ? argv[1] : "var";
    const std::string schema_path = argc > 2 ? argv[2] : "data/schema.json";
    const std::string model_path = argc > 3 ? argv[3] : "data/model/column_model.json";
    const std::string golden_path = argc > 4 ? argv[4] : "data/golden/golden.json";

    const schema::Registry registry = schema::Registry::load(schema_path);
    const std::vector<bench::Golden> golden = bench::load_golden(golden_path);
    store::Store store{state_dir};
    store.seed("data/sources.json");
    std::optional<columns::ColumnModel> neural;
    if (fileio::exists(model_path)) neural = columns::ColumnModel::load(model_path);
    const columns::ColumnModel* nn = neural.has_value() ? &*neural : nullptr;

    json::Writer w;
    w.begin_object();
    w.key("sources");
    w.begin_array();

    std::size_t fired = 0, columns_seen = 0;
    std::size_t accepted_total = 0, neural_binding = 0, neural_agreeing = 0;
    std::size_t only_with_neural = 0, only_without_neural = 0;

    for (const bench::Golden& g : golden) {
        const std::optional<store::CachedFetch> cached = store.fetch_cache(g.source_id);
        if (!cached.has_value()) continue;
        const doc::Model model = doc::build_auto(cached->content_type, cached->body);

        const schema::Mapping mapped = schema::infer_mapping(registry, model, nn);
        const schema::Mapping lexicon_only = schema::infer_mapping(registry, model, nullptr);
        const std::vector<schema::Candidate> cands =
            schema::score_candidates(registry, model, 0.0, nn);

        w.begin_object();
        emit_string(w, "source", g.source_id);

        w.key("tagger");
        w.begin_array();
        for (const std::string& label : model.labels) {
            ++columns_seen;
            const columns::Prediction p = nn->predict(label, samples_of(model, label, 3));
            const bool fires = p.confidence >= 0.7 && p.label != "none";
            if (fires) ++fired;
            w.begin_object();
            emit_string(w, "label", label);
            emit_string(w, "predicted", p.label);
            w.field("confidence", p.confidence);
            w.field("fires", fires);
            w.end_object();
        }
        w.end_array();

        w.key("mapped");
        w.begin_array();
        for (const schema::FieldMapping& fm : mapped.fields) {
            ++accepted_total;
            const schema::Candidate* c = find_candidate(cands, fm.field, fm.source_label, fm.part);
            const double neural_score = c == nullptr ? 0.0 : c->neural;
            if (neural_score > 0.0) {
                ++neural_agreeing;
                if (neural_score > fm.label_similarity) ++neural_binding;
            }
            const schema::FieldMapping* lex = find_field(lexicon_only, fm.field);
            const bool same_without =
                lex != nullptr && lex->source_label == fm.source_label && lex->part == fm.part;
            if (!same_without) ++only_with_neural;
            w.begin_object();
            emit_string(w, "field", fm.field);
            emit_string(w, "label", fm.source_label);
            w.field("part", static_cast<double>(fm.part));
            w.field("label_similarity", fm.label_similarity);
            w.field("neural", neural_score);
            w.field("value_pass_rate", fm.value_pass_rate);
            w.field("confidence", fm.confidence);
            w.field("survives_without_tagger", same_without);
            w.key("samples");
            w.begin_array();
            for (const std::string& s : samples_of(model, fm.source_label, 3)) w.string_value(s);
            w.end_array();
            w.end_object();
        }
        w.end_array();

        for (const schema::FieldMapping& fm : lexicon_only.fields) {
            const schema::FieldMapping* with = find_field(mapped, fm.field);
            if (with == nullptr || with->source_label != fm.source_label ||
                with->part != fm.part) {
                ++only_without_neural;
            }
        }

        w.key("lexicon_only_mapping");
        w.begin_array();
        for (const schema::FieldMapping& fm : lexicon_only.fields) {
            w.begin_object();
            emit_string(w, "field", fm.field);
            emit_string(w, "label", fm.source_label);
            w.field("confidence", fm.confidence);
            w.end_object();
        }
        w.end_array();

        const std::map<std::string, std::string> field_map = as_field_map(mapped);
        const bench::MappingScore score = bench::score_mapping(g, registry, field_map);

        w.key("errors");
        w.begin_array();
        for (const schema::FieldDef& field : registry.fields()) {
            const auto git = g.fields.find(field.name);
            std::vector<std::string> acceptable;
            if (git != g.fields.end()) acceptable = git->second;
            const bool listed = git != g.fields.end();
            const bool optional_ok =
                std::find(acceptable.begin(), acceptable.end(), "") != acceptable.end();
            const bool required = listed && !optional_ok;
            const auto mit = field_map.find(field.name);
            const std::string label = mit == field_map.end() ? "" : mit->second;
            const bool ok =
                listed ? std::find(acceptable.begin(), acceptable.end(), label) != acceptable.end()
                       : label.empty();
            if (ok) continue;
            const schema::FieldMapping* fm = find_field(mapped, field.name);
            w.begin_object();
            emit_string(w, "field", field.name);
            emit_string(w, "kind", std::string{schema::kind_name(field.kind)});
            emit_string(w, "got", label);
            w.key("expected");
            w.begin_array();
            for (const std::string& a : acceptable) w.string_value(a);
            w.end_array();
            emit_string(w, "type", label.empty() ? "missing" : (required ? "spurious+missing"
                                                                        : "spurious"));
            if (fm != nullptr) {
                w.field("label_similarity", fm->label_similarity);
                const schema::Candidate* c =
                    find_candidate(cands, fm->field, fm->source_label, fm->part);
                w.field("neural", c == nullptr ? 0.0 : c->neural);
                w.field("value_pass_rate", fm->value_pass_rate);
                w.field("confidence", fm->confidence);
                w.field("part", static_cast<double>(fm->part));
                w.key("samples");
                w.begin_array();
                for (const std::string& s : samples_of(model, fm->source_label, 3)) w.string_value(s);
                w.end_array();
            }
            // Every candidate the engine scored for this field, so a miss can be
            // read as "the right column never scored" or "it scored and lost".
            w.key("field_candidates");
            w.begin_array();
            for (const schema::Candidate& c : cands) {
                if (c.field != field.name) continue;
                if (c.confidence < 0.30) continue;
                w.begin_object();
                emit_string(w, "label", c.source_label);
                w.field("part", static_cast<double>(c.part));
                w.field("label_similarity", c.label_similarity);
                w.field("neural", c.neural);
                w.field("value_pass_rate", c.value_pass_rate);
                w.field("confidence", c.confidence);
                w.field("accepted", c.accepted);
                w.end_object();
            }
            w.end_array();
            // And what the expected column scored for every field, so a
            // spurious mapping can be traced to the field that outbid it.
            w.key("expected_column_candidates");
            w.begin_array();
            for (const std::string& want : acceptable) {
                if (want.empty()) continue;
                for (const schema::Candidate& c : cands) {
                    if (c.source_label != want) continue;
                    if (c.confidence < 0.30) continue;
                    w.begin_object();
                    emit_string(w, "label", c.source_label);
                    emit_string(w, "field", c.field);
                    w.field("part", static_cast<double>(c.part));
                    w.field("label_similarity", c.label_similarity);
                    w.field("neural", c.neural);
                    w.field("value_pass_rate", c.value_pass_rate);
                    w.field("confidence", c.confidence);
                    w.field("accepted", c.accepted);
                    w.end_object();
                }
                w.begin_object();
                emit_string(w, "label", want);
                w.key("samples");
                w.begin_array();
                for (const std::string& s : samples_of(model, want, 3)) w.string_value(s);
                w.end_array();
                w.end_object();
            }
            w.end_array();
            w.end_object();
        }
        w.end_array();

        w.key("score");
        w.begin_object();
        w.field("tp", static_cast<double>(score.tp));
        w.field("spurious", static_cast<double>(score.spurious));
        w.field("missing", static_cast<double>(score.missing));
        w.end_object();
        w.end_object();
    }
    w.end_array();

    w.key("totals");
    w.begin_object();
    w.field("columns_seen", static_cast<double>(columns_seen));
    w.field("tagger_fired", static_cast<double>(fired));
    w.field("accepted_mappings", static_cast<double>(accepted_total));
    w.field("accepted_with_neural_agreeing", static_cast<double>(neural_agreeing));
    w.field("accepted_with_neural_above_lexicon", static_cast<double>(neural_binding));
    w.field("accepted_only_with_tagger", static_cast<double>(only_with_neural));
    w.field("accepted_only_without_tagger", static_cast<double>(only_without_neural));
    w.end_object();
    w.end_object();

    std::printf("%s\n", w.take().c_str());
    return 0;
}
