#include "commands.hpp"

#include "render.hpp"

#include "dd/core/core.hpp"
#include "dd/engine/bench.hpp"
#include "dd/engine/events.hpp"
#include "dd/ml/llm.hpp"
#include "dd/parse/document.hpp"

#include <algorithm>
#include <cstdio>
#include <istream>
#include <map>
#include <optional>

namespace dd::cli {
namespace {

using render::meter;
using render::paint;
using render::section;
using render::stamp;

std::string pct(double v) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%.0f%%", v * 100.0);
    return buffer;
}

std::optional<doc::Model> model_from_cache(store::Store& store, const std::string& source_id) {
    const std::optional<store::CachedFetch> cached = store.fetch_cache(source_id);
    if (!cached.has_value()) return std::nullopt;
    return doc::build_auto(cached->content_type, cached->body);
}

std::vector<std::string> sample_values(const doc::Model& model, const std::string& label,
                                       std::size_t limit) {
    std::vector<std::string> out;
    for (const doc::RawRecord& record : model.records) {
        if (out.size() >= limit) break;
        const doc::Cell* cell = record.find(label);
        if (cell == nullptr || cell->value.empty()) continue;
        std::string value = cell->value;
        if (value.size() > 40) value = value.substr(0, 39) + "…";
        out.push_back(std::move(value));
    }
    return out;
}

// The most recent value of one detail field across a property's events.
std::string latest_detail(const std::vector<events::PropertyEvent>& evs,
                          const std::string& field) {
    for (auto it = evs.rbegin(); it != evs.rend(); ++it) {
        const auto found = it->details.find(field);
        if (found != it->details.end()) return found->second;
    }
    return {};
}

std::string first_role_value(const schema::Registry& registry,
                             const std::vector<events::PropertyEvent>& evs,
                             std::string_view role) {
    for (const schema::FieldDef* field : registry.with_role(role)) {
        const std::string value = latest_detail(evs, field->name);
        if (!value.empty()) return value;
    }
    return {};
}

bool ask_yes_no(std::istream& in, const std::string& question) {
    std::printf("%s [y/n] ", question.c_str());
    std::fflush(stdout);
    std::string answer;
    if (!std::getline(in, answer)) return false;
    const std::string cleaned = str::to_lower(str::trim(answer));
    return cleaned == "y" || cleaned == "yes";
}

} // namespace

void counties(store::Store& store) {
    struct Row {
        std::string jurisdiction;
        std::size_t sources = 0;
        std::size_t ok = 0;
    };
    std::vector<Row> rows;
    for (const store::Source& s : store.sources()) {
        auto it = std::find_if(rows.begin(), rows.end(),
                               [&](const Row& r) { return r.jurisdiction == s.jurisdiction; });
        if (it == rows.end()) {
            rows.push_back(Row{s.jurisdiction, 0, 0});
            it = rows.end() - 1;
        }
        ++it->sources;
        const std::vector<store::RunRecord> last = store.runs(1, s.id);
        if (!last.empty() && last.front().ok) ++it->ok;
    }

    std::map<std::string, std::size_t> properties;
    std::map<std::string, std::size_t> events_count;
    for (const std::string& key : store.property_keys()) {
        const std::string slug = key.substr(0, key.find('|'));
        ++properties[slug];
        events_count[slug] += store.events_for(key).size();
    }

    section("Counties");
    std::vector<std::vector<std::string>> table;
    for (const Row& r : rows) {
        const std::string slug = str::slug(r.jurisdiction);
        table.push_back({r.jurisdiction, std::to_string(r.sources),
                         r.ok == r.sources ? stamp("ok") : std::to_string(r.ok) + " ok",
                         std::to_string(properties[slug]), std::to_string(events_count[slug])});
    }
    render::table({"county", "sources", "ingested", "properties", "events"}, table);
    std::printf("  %s\n", paint("dim", "county NAME lists its properties; run all ingests "
                                       "everything").c_str());
}

void county_properties(store::Store& store, const schema::Registry& registry,
                       const std::string& county) {
    const std::string wanted = str::slug(county);
    std::vector<std::string> keys;
    for (const std::string& key : store.property_keys()) {
        const std::string slug = key.substr(0, key.find('|'));
        if (slug == wanted || str::contains(slug, wanted)) keys.push_back(key);
    }
    if (keys.empty()) {
        std::printf("  no properties for '%s'; run its sources first (run all)\n",
                    county.c_str());
        return;
    }

    section("Properties");
    std::vector<std::vector<std::string>> rows;
    for (const std::string& key : keys) {
        const std::vector<events::PropertyEvent> evs = store.events_for(key);
        const events::Lifecycle life = events::reduce(evs);
        std::string amount;
        for (auto it = evs.rbegin(); it != evs.rend(); ++it) {
            if (it->amount > 0.0) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.2f", it->amount);
                amount = buffer;
                break;
            }
        }
        rows.push_back({first_role_value(registry, evs, "parcel"),
                        first_role_value(registry, evs, "owner"),
                        first_role_value(registry, evs, "address"),
                        stamp(std::string{events::state_name(life.state)}), amount,
                        std::to_string(evs.size()), evs.back().event_date});
    }
    render::table({"parcel", "owner", "address", "state", "amount", "events", "last event"},
                  rows);
    std::printf("  %s\n",
                paint("dim", std::to_string(keys.size()) + " properties, filled from the "
                             "schema's identity and role fields").c_str());
}

void show_mapping(store::Store& store, pipeline::Pipeline& pipeline,
                  const std::string& source_id) {
    const store::SourceState state = store.source_state(source_id);
    if (!state.has_mapping) {
        std::printf("  '%s' has no learned mapping yet; run it first\n", source_id.c_str());
        return;
    }
    const std::optional<doc::Model> model = model_from_cache(store, source_id);

    section("Mapping for " + source_id);
    std::vector<std::vector<std::string>> rows;
    for (const schema::FieldMapping& fm : state.mapping.fields) {
        std::string samples;
        if (model.has_value()) {
            samples = str::join(sample_values(*model, fm.source_label, 2), "  |  ");
        }
        rows.push_back({fm.field, "<- " + fm.source_label,
                        fm.reformatted ? stamp("reformatted") : "", meter(fm.confidence),
                        samples});
    }
    render::table({"canonical", "source label", "", "confidence", "sample source values"}, rows);

    if (model.has_value()) {
        std::vector<std::string> unmatched;
        for (const std::string& label : model->labels) {
            bool used = false;
            for (const schema::FieldMapping& fm : state.mapping.fields) {
                if (fm.source_label == label) used = true;
            }
            if (!used) unmatched.push_back(label);
        }
        if (!unmatched.empty()) {
            std::printf("\n  %s\n", paint("dim", "unmatched source labels:").c_str());
            for (const std::string& label : unmatched) {
                const std::string samples =
                    str::join(sample_values(*model, label, 2), "  |  ");
                std::printf("    %-28s %s\n", label.c_str(), paint("dim", samples).c_str());
            }
        }
    }
    if (!state.overrides.empty()) {
        std::printf("\n  %s\n", paint("dim", "operator overrides in effect:").c_str());
        for (const auto& [field, label] : state.overrides) {
            std::printf("    %s -> %s\n", field.c_str(),
                        label.empty() ? "(unmapped)" : label.c_str());
        }
    }
    (void)pipeline;
}

void review(store::Store& store, pipeline::Pipeline& pipeline, const std::string& source_id,
            std::istream& in) {
    const std::optional<doc::Model> model = model_from_cache(store, source_id);
    if (!model.has_value()) {
        std::printf("  no cached fetch for '%s'; run it first\n", source_id.c_str());
        return;
    }
    store::SourceState state = store.source_state(source_id);
    const std::vector<schema::Candidate> candidates =
        schema::score_candidates(pipeline.registry(), *model, 0.50);

    section("Review " + source_id);
    std::size_t decisions = 0;

    for (const schema::Candidate& c : candidates) {
        if (state.overrides.count(c.field) != 0) continue; // operator already decided
        const std::string samples = str::join(sample_values(*model, c.source_label, 3), "  |  ");

        if (c.accepted && c.confidence < 0.85) {
            std::printf("\n  %s %s <- %s at %s confidence%s\n", stamp("pending").c_str(),
                        c.field.c_str(), c.source_label.c_str(), pct(c.confidence).c_str(),
                        c.reformatted ? " (values need extraction)" : "");
            std::printf("    samples: %s\n", paint("dim", samples).c_str());
            if (ask_yes_no(in, "    keep this match?")) {
                state.overrides[c.field] = c.source_label;
            } else {
                state.overrides[c.field] = "";
            }
            ++decisions;
        } else if (!c.accepted && state.mapping.find(c.field) == nullptr) {
            std::printf("\n  %s %s <- %s at %s confidence (held back automatically)\n",
                        stamp("review").c_str(), c.field.c_str(), c.source_label.c_str(),
                        pct(c.confidence).c_str());
            std::printf("    samples: %s\n", paint("dim", samples).c_str());
            if (ask_yes_no(in, "    map it anyway?")) {
                state.overrides[c.field] = c.source_label;
                ++decisions;
            }
        }
    }

    if (decisions == 0) {
        std::printf("  every match is confident; nothing to review\n");
        return;
    }
    store.save_source_state(state);
    const std::optional<store::Source> source = store.find_source(source_id);
    if (source.has_value()) {
        const store::RunRecord run = pipeline.run_source(*source);
        std::printf("\n  %s re-ran with your decisions: %lld records at %s extraction\n",
                    run.ok ? stamp("ok").c_str() : stamp("failed").c_str(),
                    static_cast<long long>(run.records_extracted),
                    pct(run.extraction_rate).c_str());
    }
}

void model_status(const classify::Classifier& classifier) {
    section("Model");
    char alpha[16];
    std::snprintf(alpha, sizeof(alpha), "%.2f", classifier.bayes().alpha());
    render::kv({
        {"algorithm", "multinomial naive Bayes, Lidstone smoothing"},
        {"smoothing alpha", alpha},
        {"training examples", std::to_string(classifier.example_count())},
        {"classes", std::to_string(classifier.bayes().class_count())},
        {"vocabulary", std::to_string(classifier.bayes().vocabulary_size()) + " tokens"},
        {"trained", classifier.trained_at()},
        {"holdout accuracy", meter(classifier.trained_accuracy()) + "  (leave-one-out)"},
    });
    std::vector<std::vector<std::string>> rows;
    for (const model::ClassSummary& c : classifier.bayes().summarize(5)) {
        std::vector<std::string> tokens;
        for (const model::TokenWeight& t : c.top_tokens) tokens.push_back(t.token);
        rows.push_back({c.name, std::to_string(c.documents), str::join(tokens, ", ")});
    }
    render::table({"class", "examples", "top vocabulary by lift"}, rows);
}

int train(pipeline::Pipeline* pipeline, const std::string& corpus_dir,
          const std::string& model_path, double alpha, bool sweep) {
    struct Attempt {
        double alpha;
        classify::TrainReport report;
        classify::Classifier classifier;
    };
    std::vector<double> alphas = sweep ? std::vector<double>{0.1, 0.3, 0.5, 1.0, 2.0}
                                       : std::vector<double>{alpha};

    std::optional<Attempt> best;
    section(sweep ? "Alpha sweep (leave-one-out per value)" : "Train");
    for (double a : alphas) {
        classify::TrainReport report;
        classify::Classifier classifier =
            classify::Classifier::train_from_corpus(corpus_dir, &report, a);
        std::printf("  alpha %-5.2f accuracy %s  (%zu examples, %zu classes)\n", a,
                    meter(report.leave_one_out_accuracy).c_str(), report.examples,
                    report.classes);
        if (!best.has_value() ||
            report.leave_one_out_accuracy > best->report.leave_one_out_accuracy) {
            best = Attempt{a, std::move(report), std::move(classifier)};
        }
    }

    // Per-class results and confusion from the winning run.
    std::map<std::string, std::pair<std::size_t, std::size_t>> per_class; // correct, total
    std::map<std::string, std::size_t> confusion;
    for (const classify::LooPrediction& p : best->report.predictions) {
        auto& [correct, total] = per_class[p.actual];
        ++total;
        if (p.actual == p.predicted) ++correct;
        else ++confusion[p.actual + " misread as " + p.predicted];
    }
    section("Validation at alpha " + std::to_string(best->alpha).substr(0, 4));
    std::vector<std::vector<std::string>> rows;
    for (const auto& [name, counts] : per_class) {
        rows.push_back({name, std::to_string(counts.first) + "/" + std::to_string(counts.second),
                        meter(static_cast<double>(counts.first) /
                              static_cast<double>(counts.second))});
    }
    render::table({"class", "correct", "accuracy"}, rows);
    if (confusion.empty()) {
        std::printf("  no confusions: every held-out document classified correctly\n");
    } else {
        for (const auto& [pair, count] : confusion) {
            std::printf("  %s %s (x%zu)\n", stamp("review").c_str(), pair.c_str(), count);
        }
    }

    if (best->report.leave_one_out_accuracy < 0.85) {
        std::printf("%s accuracy below 0.85; model not saved\n", stamp("failed").c_str());
        return 1;
    }
    best->classifier.save(model_path);
    std::printf("\n  %s saved to %s", stamp("ok").c_str(), model_path.c_str());
    if (pipeline != nullptr) {
        pipeline->set_classifier(best->classifier);
        std::printf(" and live-swapped into this session");
    }
    std::printf("\n");
    return 0;
}

namespace {

std::vector<std::string> class_names(const classify::Classifier& classifier) {
    std::vector<std::string> out;
    for (const model::ClassSummary& c : classifier.bayes().summarize(1)) out.push_back(c.name);
    return out;
}

std::map<std::string, std::string> as_field_map(const schema::Mapping& mapping) {
    std::map<std::string, std::string> out;
    for (const schema::FieldMapping& fm : mapping.fields) out[fm.field] = fm.source_label;
    return out;
}

struct BenchTotals {
    std::size_t docs = 0;
    std::size_t cls_ok = 0;
    bench::MappingScore mapping;
    double ms = 0.0;

    void add(bool ok, const bench::MappingScore& s, double elapsed) {
        ++docs;
        if (ok) ++cls_ok;
        mapping.tp += s.tp;
        mapping.spurious += s.spurious;
        mapping.missing += s.missing;
        ms += elapsed;
    }
};

void print_totals(const char* who, const BenchTotals& t, const std::string& cost) {
    std::printf("  %s: classification %zu/%zu, mapping precision %s recall %s F1 %s, "
                "%.0f ms for %zu documents, cost %s\n",
                who, t.cls_ok, t.docs, pct(t.mapping.precision()).c_str(),
                pct(t.mapping.recall()).c_str(), pct(t.mapping.f1()).c_str(), t.ms, t.docs,
                cost.c_str());
}

} // namespace

int bench(store::Store& store, pipeline::Pipeline& pipeline, const std::string& golden_path,
          bool with_llm) {
    const std::vector<bench::Golden> golden = bench::load_golden(golden_path);
    const std::vector<std::string> classes = class_names(pipeline.classifier());

    section("Engine vs answer key (" + std::to_string(golden.size()) + " hand-verified sources)");
    BenchTotals engine_totals;
    std::vector<std::vector<std::string>> rows;
    struct Doc {
        const bench::Golden* golden;
        doc::Model model;
        std::string url;
    };
    std::vector<Doc> docs;
    for (const bench::Golden& g : golden) {
        const std::optional<store::CachedFetch> cached = store.fetch_cache(g.source_id);
        if (!cached.has_value()) {
            rows.push_back({g.source_id, stamp("failed"), "no cached bytes: run it first",
                            "", "", ""});
            continue;
        }
        const std::optional<store::Source> source = store.find_source(g.source_id);
        Doc d{&g, doc::build_auto(cached->content_type, cached->body),
              source.has_value() ? source->url : ""};

        const Stopwatch watch;
        const classify::Prediction prediction =
            pipeline.classifier().classify(d.model, d.url);
        const schema::Mapping mapping = schema::infer_mapping(pipeline.registry(), d.model);
        const double elapsed = watch.elapsed_ms();

        const bool cls_ok = bench::classification_ok(g, prediction.label);
        const bench::MappingScore score =
            bench::score_mapping(g, pipeline.registry(), as_field_map(mapping));
        engine_totals.add(cls_ok, score, elapsed);
        rows.push_back({g.source_id, cls_ok ? stamp("ok") : stamp("failed"), prediction.label,
                        std::to_string(score.tp) + "/" + std::to_string(score.spurious) + "/" +
                            std::to_string(score.missing),
                        meter(score.f1()),
                        std::to_string(static_cast<int>(elapsed * 1000.0)) + " us"});
        docs.push_back(std::move(d));
    }
    render::table({"source", "class", "predicted", "ok/spur/miss", "mapping F1", "compute"},
                  rows);
    print_totals("engine", engine_totals, "$0.00");

    if (!with_llm) {
        std::printf("  %s\n",
                    paint("dim", "bench llm runs the same key through an OpenAI-compatible "
                                 "baseline (DD_LLM_ENDPOINT/KEY/MODEL)").c_str());
        return 0;
    }

    const llm::Config config = llm::config_from_env();
    if (!config.ready()) {
        std::printf("\n  %s set DD_LLM_MODEL and DD_LLM_KEY (and DD_LLM_ENDPOINT for a "
                    "non-OpenAI provider) to run the baseline\n",
                    stamp("failed").c_str());
        return 1;
    }

    section("LLM baseline (" + config.model + ") vs the same answer key");
    BenchTotals llm_totals;
    std::int64_t tokens_in = 0;
    std::int64_t tokens_out = 0;
    std::vector<std::vector<std::string>> llm_rows;
    for (const Doc& d : docs) {
        const llm::Completion completion =
            llm::complete(config, bench::llm_prompt(pipeline.registry(), classes, d.model));
        if (!completion.ok) {
            llm_rows.push_back({d.golden->source_id, stamp("failed"), completion.error, "", "",
                                ""});
            llm_totals.add(false, bench::MappingScore{}, completion.total_ms);
            continue;
        }
        tokens_in += completion.tokens_in;
        tokens_out += completion.tokens_out;
        const bench::LlmAnswer answer = bench::parse_llm_answer(completion.text);
        if (!answer.ok) {
            llm_rows.push_back({d.golden->source_id, stamp("failed"), answer.error, "", "", ""});
            llm_totals.add(false, bench::MappingScore{}, completion.total_ms);
            continue;
        }
        const bool cls_ok = bench::classification_ok(*d.golden, answer.classification);
        const bench::MappingScore score =
            bench::score_mapping(*d.golden, pipeline.registry(), answer.mapping);
        llm_totals.add(cls_ok, score, completion.total_ms);
        llm_rows.push_back({d.golden->source_id, cls_ok ? stamp("ok") : stamp("failed"),
                            answer.classification,
                            std::to_string(score.tp) + "/" + std::to_string(score.spurious) +
                                "/" + std::to_string(score.missing),
                            meter(score.f1()),
                            std::to_string(static_cast<int>(completion.total_ms)) + " ms"});
    }
    render::table({"source", "class", "predicted", "ok/spur/miss", "mapping F1", "latency"},
                  llm_rows);

    std::string cost = std::to_string(tokens_in) + " in / " + std::to_string(tokens_out) +
                       " out tokens";
    if (config.price_in > 0.0 || config.price_out > 0.0) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "$%.4f",
                      (static_cast<double>(tokens_in) * config.price_in +
                       static_cast<double>(tokens_out) * config.price_out) /
                          1e6);
        cost += " = ";
        cost += buffer;
    }
    print_totals("llm", llm_totals, cost);
    print_totals("engine", engine_totals, "$0.00");
    return 0;
}

} // namespace dd::cli
