#include "commands.hpp"

#include "render.hpp"

#include "dd/core/core.hpp"
#include "dd/engine/bench.hpp"
#include "dd/engine/compile.hpp"
#include "dd/engine/harvest.hpp"
#include "dd/engine/events.hpp"
#include "dd/net/crawl.hpp"
#include "dd/parse/document.hpp"

#include <algorithm>
#include <cstdio>
#include <istream>
#include <map>
#include <set>
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

enum class Answer { Yes, No, Abort };

Answer ask_yes_no(std::istream& in, const std::string& question) {
    while (true) {
        std::printf("%s [y/n] ", question.c_str());
        std::fflush(stdout);
        std::string answer;
        if (!std::getline(in, answer)) return Answer::Abort;
        const std::string cleaned = str::to_lower(str::trim(answer));
        if (cleaned == "y" || cleaned == "yes") return Answer::Yes;
        if (cleaned == "n" || cleaned == "no") return Answer::No;
        std::printf("    please answer y or n (end input to abort)\n");
    }
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

namespace {
std::string fmt_money(double v) {
    char raw[32];
    std::snprintf(raw, sizeof(raw), "%.0f", v);
    std::string digits{raw};
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return "$" + out;
}

std::string clip(std::string s, std::size_t n) {
    if (s.size() > n) s = s.substr(0, n - 1) + "…";
    return s;
}

std::string field_or(const compile::Property& p, const char* field) {
    const auto it = p.fields.find(field);
    return it == p.fields.end() ? "" : it->second.value;
}
} // namespace

void county_properties(store::Store& store, const schema::Registry& registry,
                       const std::string& county, bool include_all) {
    const std::vector<compile::Property> properties =
        compile::county(store, registry, county);
    if (properties.empty()) {
        std::printf("  no properties for '%s'; run its sources first (run all)\n",
                    county.c_str());
        return;
    }

    section("Properties");
    std::vector<std::vector<std::string>> table;
    std::size_t hidden = 0;
    std::size_t merged = 0;
    std::size_t conflicts = 0;
    std::size_t cross_source = 0;
    for (const compile::Property& p : properties) {
        conflicts += p.conflicts.size();
        if (p.keys.size() > 1) ++merged;
        const std::string address = field_or(p, "address");
        if (!p.locates_a_building && !include_all) {
            ++hidden;
            continue;
        }
        std::string change;
        if (p.assessed > 0.0 && p.assessed_previous > 0.0) {
            const double delta = (p.assessed - p.assessed_previous) / p.assessed_previous;
            if (std::abs(delta) >= 0.005) {
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "%+.0f%%", delta * 100.0);
                change = buffer;
            }
        }
        std::string ratio;
        if (p.due > 0.0 && p.assessed > 0.0) {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "%.1fx", p.due / p.assessed);
            ratio = buffer;
        }
        std::set<std::string> sources;
        for (const events::PropertyEvent& e : p.events) sources.insert(e.source_id);
        if (sources.size() > 1) ++cross_source;
        std::string parcel = field_or(p, "parcel_id");
        if (p.keys.size() > 1) parcel += "*";
        const events::PropertyEvent& last = p.events.back();
        std::string last_event{events::kind_name(last.kind)};
        if (!last.event_date.empty()) last_event += " " + last.event_date;
        table.push_back({parcel, clip(field_or(p, "owner"), 26), clip(address, 26),
                         stamp(std::string{events::state_name(p.state)}),
                         p.due > 0.0 ? fmt_money(p.due) : "",
                         p.assessed > 0.0 ? fmt_money(p.assessed) : "", change, ratio,
                         p.violations > 0 ? std::to_string(p.violations) : "",
                         std::to_string(sources.size()), last_event});
    }
    render::table({"parcel", "owner", "address", "lifecycle", "owed", "assessed", "yoy",
                   "debt/val", "viol", "src", "last event"},
                  table);
    std::string note = std::to_string(table.size()) + " properties, most distressed first; " +
                       std::to_string(cross_source) + " corroborated by more than one source";
    if (merged > 0) {
        note += "; " + std::to_string(merged) + " merged across id spaces (*)";
    }
    if (conflicts > 0) {
        note += "; " + std::to_string(conflicts) +
                " conflicts resolved by measured source trust (see export)";
    }
    if (hidden > 0) {
        note += "; " + std::to_string(hidden) +
                " block-level or unlocatable records hidden ('county " + county + " all')";
    }
    std::printf("  %s\n", paint("dim", note).c_str());
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
        schema::score_candidates(pipeline.registry(), *model, 0.50,
                                 pipeline.column_model());

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
            const Answer answer = ask_yes_no(in, "    keep this match?");
            if (answer == Answer::Abort) {
                std::printf("\n  review aborted; nothing saved\n");
                return;
            }
            state.overrides[c.field] = answer == Answer::Yes ? c.source_label : "";
            ++decisions;
        } else if (!c.accepted && state.mapping.find(c.field) == nullptr) {
            std::printf("\n  %s %s <- %s at %s confidence (held back automatically)\n",
                        stamp("review").c_str(), c.field.c_str(), c.source_label.c_str(),
                        pct(c.confidence).c_str());
            std::printf("    samples: %s\n", paint("dim", samples).c_str());
            const Answer answer = ask_yes_no(in, "    map it anyway?");
            if (answer == Answer::Abort) {
                std::printf("\n  review aborted; nothing saved\n");
                return;
            }
            if (answer == Answer::Yes) {
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

void model_status(const classify::Classifier& classifier,
                  const columns::ColumnModel* column_model) {
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

    section("Column transformer");
    if (column_model == nullptr) {
        std::printf("  not loaded; harvest then 'train columns' to build one\n");
        return;
    }
    const columns::Hyper& h = column_model->hyper();
    char arch[96];
    std::snprintf(arch, sizeof(arch), "%d layers, d_model %d, %d heads, ffn %d, seq %d",
                  h.layers, h.d_model, h.heads, h.d_ffn, h.seq_len);
    render::kv({
        {"algorithm", "byte-level transformer encoder (in-repo, no deps)"},
        {"architecture", arch},
        {"parameters", std::to_string(column_model->parameter_count())},
        {"classes", std::to_string(column_model->classes().size()) +
                        " canonical fields + none"},
    });
}

int harvest_docs(const std::string& corpus_dir, std::size_t datasets_per_query) {
    section("Growing the document corpus from live portals");
    const std::map<std::string, std::size_t> written = harvest::grow_corpus(
        corpus_dir, datasets_per_query,
        [](const std::string& line) { std::printf("  %s\n", line.c_str()); });
    std::size_t total = 0;
    for (const auto& [label, count] : written) total += count;
    if (total == 0) {
        std::printf("  %s nothing new; corpus unchanged\n", stamp("unchanged").c_str());
        return 1;
    }
    std::printf("  %s %zu real datasets added; run 'train sweep' and prune any file the "
                "confusion table flags\n",
                stamp("ok").c_str(), total);
    return 0;
}

int train(pipeline::Pipeline* pipeline, const std::string& corpus_dir,
          const std::string& model_path, double alpha, bool sweep, double min_accuracy) {
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

    std::map<std::string, std::pair<std::size_t, std::size_t>> per_class; // correct, total
    std::map<std::string, std::size_t> confusion;
    std::vector<classify::LooPrediction> disagreements;
    for (const classify::LooPrediction& p : best->report.predictions) {
        auto& [correct, total] = per_class[p.actual];
        ++total;
        if (p.actual == p.predicted) ++correct;
        else {
            ++confusion[p.actual + " misread as " + p.predicted];
            disagreements.push_back(p);
        }
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
        std::printf("\n  %s\n", paint("dim", "documents that disagree with their folder:").c_str());
        std::size_t shown = 0;
        for (const classify::LooPrediction& p : disagreements) {
            if (++shown > 12) {
                std::printf("    %s\n",
                            paint("dim", "... " + std::to_string(disagreements.size() - 12) +
                                             " more").c_str());
                break;
            }
            std::printf("    %-52s %s -> %s\n", p.source.c_str(), p.actual.c_str(),
                        p.predicted.c_str());
        }
    }

    if (best->report.leave_one_out_accuracy < min_accuracy) {
        std::printf("%s accuracy below %.2f; model not saved\n", stamp("failed").c_str(),
                    min_accuracy);
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

int harvest(const schema::Registry& registry, const std::string& corpus_path,
            std::size_t datasets_per_query) {
    harvest::Options options = harvest::default_options();
    if (datasets_per_query > 0) options.datasets_per_query = datasets_per_query;
    section("Harvesting real government columns (Socrata discovery API)");
    harvest::Stats stats;
    const std::vector<columns::Example> examples = harvest::run(
        registry, options,
        [](const std::string& line) { std::printf("  %s\n", line.c_str()); }, &stats);
    if (examples.empty()) {
        std::printf("  %s nothing harvested\n", stamp("failed").c_str());
        return 1;
    }
    // Merge rather than replace: a harvest is one sample of the portals, and
    // clobbering throws away every column an earlier run paid to fetch.
    std::vector<columns::Example> merged;
    std::set<std::pair<std::string, std::string>> seen;
    try {
        for (const columns::Example& e : columns::load_corpus(corpus_path)) {
            if (seen.insert({e.domain, e.name}).second) merged.push_back(e);
        }
    } catch (const Error&) {
        // No corpus yet, or an unreadable one: this harvest becomes the corpus.
    }
    const std::size_t before = merged.size();
    for (const columns::Example& e : examples) {
        if (seen.insert({e.domain, e.name}).second) merged.push_back(e);
    }
    std::remove(corpus_path.c_str());
    columns::append_corpus(corpus_path, merged);
    render::kv({
        {"datasets sampled", std::to_string(stats.datasets_sampled) + " of " +
                                 std::to_string(stats.datasets_seen) + " seen"},
        {"portals", std::to_string(stats.domains)},
        {"columns this run", std::to_string(examples.size())},
        {"corpus size", std::to_string(merged.size()) + " (" +
                            std::to_string(merged.size() - before) + " new, " +
                            std::to_string(before) + " kept)"},
        {"lexicon-labeled", std::to_string(stats.labeled)},
        {"validator-labeled", std::to_string(stats.validator_labeled)},
        {"none", std::to_string(stats.none)},
        {"masked near-misses", std::to_string(stats.masked)},
        {"corpus", corpus_path},
    });
    std::printf("  %s\n", paint("dim", "labels are exact lexicon hits only; the model must "
                                        "generalise to everything else").c_str());
    return 0;
}

int train_columns(pipeline::Pipeline* pipeline, const std::string& corpus_path,
                  const std::string& model_path, int epochs) {
    const std::vector<columns::Example> corpus = columns::load_corpus(corpus_path);
    if (corpus.empty()) {
        std::printf("  no corpus at %s; run harvest first\n", corpus_path.c_str());
        return 1;
    }
    std::vector<columns::Example> train_set;
    std::vector<columns::Example> holdout;
    columns::split_by_domain(corpus, 5, &train_set, &holdout);

    columns::ColumnModel model{columns::Hyper{}};
    columns::TrainConfig config;
    config.epochs = epochs;
    section("Training the column transformer");
    render::kv({
        {"train", std::to_string(train_set.size()) + " columns"},
        {"holdout", std::to_string(holdout.size()) + " columns (unseen portals)"},
        {"epochs", std::to_string(config.epochs)},
    });
    const Stopwatch watch;
    const columns::TrainReport report = model.train(train_set, holdout, config);
    std::printf("  %zu parameters, %.1f s\n", report.parameters, watch.elapsed_ms() / 1000.0);
    for (std::size_t i = 0; i < report.epoch_loss.size(); ++i) {
        std::printf("  epoch %zu  loss %.4f\n", i + 1, report.epoch_loss[i]);
    }

    section("Holdout results (portals the model never saw)");
    std::vector<std::vector<std::string>> rows;
    std::vector<columns::ClassResult> per_class = report.per_class;
    std::sort(per_class.begin(), per_class.end(),
              [](const columns::ClassResult& a, const columns::ClassResult& b) {
                  return a.label < b.label;
              });
    for (const columns::ClassResult& r : per_class) {
        rows.push_back({r.label, std::to_string(r.correct) + "/" + std::to_string(r.total),
                        pct(r.precision()), pct(r.recall()), meter(r.f1())});
    }
    render::table({"class", "correct", "precision", "recall", "f1"}, rows);
    std::printf("  macro F1 %s over %zu field classes; %s on the %zu columns that carry a field\n",
                pct(report.macro_f1).c_str(),
                report.per_class.empty() ? 0 : report.per_class.size() - 1,
                pct(report.positive_accuracy).c_str(), report.positive_examples);
    std::printf("  %s\n",
                paint("dim", "overall accuracy " + pct(report.holdout_accuracy) + " on " +
                                 std::to_string(report.holdout_examples) +
                                 " columns counts the 'none' majority: macro F1 is the honest one")
                    .c_str());

    if (report.macro_f1 < 0.50) {
        std::printf("  %s macro F1 below 0.50: not saved\n", stamp("failed").c_str());
        return 1;
    }
    model.save(model_path);
    std::printf("  %s saved to %s\n", stamp("ok").c_str(), model_path.c_str());
    if (pipeline != nullptr) {
        pipeline->set_column_model(std::move(model));
        std::printf("  %s\n", paint("dim", "live matcher now uses this model").c_str());
    }
    return 0;
}

int export_county(store::Store& store, const schema::Registry& registry,
                  const std::string& county, const std::string& out_path) {
    const std::string payload =
        compile::render_county_json(county, compile::county(store, registry, county));
    if (out_path.empty()) {
        std::printf("%s\n", payload.c_str());
        return 0;
    }
    fileio::write_file_atomic(out_path, payload);
    std::printf("  %s wrote %zu bytes to %s\n", stamp("ok").c_str(), payload.size(),
                out_path.c_str());
    return 0;
}

int freshness(store::Store& store, double stale_hours) {
    section("Freshness");
    const std::string now = timeutil::iso_now();
    std::vector<std::vector<std::string>> rows;
    std::size_t stale = 0;
    std::size_t live = 0;
    for (const store::Source& s : store.sources()) {
        const std::vector<store::RunRecord> last = store.runs(1, s.id);
        if (last.empty()) {
            rows.push_back({s.id, "never run", "", "", stamp("pending")});
            continue;
        }
        const store::RunRecord& run = last.front();
        const double fetch_age = timeutil::hours_between(run.started_at, now);
        const double record_age = run.newest_record_date.empty()
                                      ? -1.0
                                      : timeutil::hours_between(run.newest_record_date, now);
        char fetched[32];
        std::snprintf(fetched, sizeof(fetched), "%.1f h ago", fetch_age);
        std::string record_col = run.newest_record_date;
        std::string age_col;
        std::string verdict;
        if (!run.ok) {
            verdict = stamp("failed");
        } else if (run.newest_record_date.empty()) {
            verdict = stamp("unchanged");
            age_col = "undated";
        } else if (record_age < 0.0) {
            verdict = stamp("ok");  // scheduled ahead of now, as permits are
            age_col = "future";
            ++live;
        } else {
            char age[32];
            std::snprintf(age, sizeof(age), "%.0f h", record_age);
            age_col = age;
            if (record_age > stale_hours) {
                verdict = stamp("review");
                ++stale;
            } else {
                verdict = stamp("ok");
                ++live;
            }
        }
        rows.push_back({s.id, fetched, record_col, age_col, verdict});
    }
    render::table({"source", "last fetched", "newest record", "record age", ""}, rows);
    char note[160];
    std::snprintf(note, sizeof(note),
                  "%zu sources current within %.0f h, %zu behind it; a fetch that succeeds "
                  "with old rows is the failure this catches",
                  live, stale_hours, stale);
    std::printf("  %s\n", paint("dim", note).c_str());
    return stale > 0 ? 1 : 0;
}

int crawl_site(const schema::Registry& registry, const classify::Classifier& classifier,
               const columns::ColumnModel* column_model, const std::string& seed,
               std::size_t max_pages, std::size_t max_depth) {
    section("Crawling " + seed);
    crawl::Options options;
    options.max_pages = max_pages;
    options.max_depth = max_depth;

    struct Found {
        std::string url;
        std::string classification;
        std::size_t records = 0;
        double extraction = 0.0;
        std::size_t fields = 0;
    };
    std::vector<Found> found;
    std::size_t parsed = 0;

    const crawl::Stats stats = crawl::crawl(
        seed, options,
        [&](const crawl::Page& page) {
            const doc::Model model = doc::build_auto(page.content_type, page.body);
            if (model.records.empty()) return true;
            ++parsed;
            const schema::Mapping mapping = schema::infer_mapping(registry, model, column_model);
            if (mapping.fields.empty()) return true;  // no identity evidence: not records
            const schema::ExtractionResult extraction =
                schema::apply_mapping(registry, mapping, model);
            if (extraction.rate <= 0.0) return true;
            found.push_back(Found{page.url, classifier.classify(model, page.url).label,
                                  model.records.size(), extraction.rate, mapping.fields.size()});
            return true;
        },
        [](const std::string& line) { std::printf("  %s\n", line.c_str()); });

    render::kv({
        {"pages fetched", std::to_string(stats.fetched)},
        {"pages with records", std::to_string(parsed)},
        {"pages that mapped", std::to_string(found.size())},
        {"refused by robots", std::to_string(stats.skipped_robots)},
        {"duplicates skipped", std::to_string(stats.skipped_duplicate)},
        {"failed", std::to_string(stats.failed)},
        {"fetch time", std::to_string(static_cast<long long>(stats.fetch_ms)) + " ms"},
    });

    if (found.empty()) {
        std::printf("  %s nothing on this site extracted against the schema\n",
                    stamp("failed").c_str());
        return 1;
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return a.records * a.extraction > b.records * b.extraction;
    });
    std::vector<std::vector<std::string>> rows;
    for (const Found& f : found) {
        std::string url = f.url;
        if (url.size() > 58) url = url.substr(0, 57) + "…";
        rows.push_back({url, f.classification, std::to_string(f.records),
                        std::to_string(f.fields), meter(f.extraction)});
    }
    render::table({"page", "classified", "records", "fields", "extraction"}, rows);
    std::printf("  %s\n", paint("dim", "'add \"County\" \"Name\" URL' turns one of these into a "
                                       "source").c_str());
    return 0;
}

int catalog(store::Store& store, std::size_t datasets_per_query, bool add) {
    section("Country-wide source discovery");
    const std::vector<harvest::Discovered> found = harvest::discover(
        datasets_per_query, [](const std::string& line) { std::printf("  %s\n", line.c_str()); });
    if (found.empty()) {
        std::printf("  %s catalog returned nothing\n", stamp("failed").c_str());
        return 1;
    }
    std::set<std::string> existing;
    for (const store::Source& s : store.sources()) existing.insert(s.url);

    std::map<std::string, std::size_t> by_jurisdiction;
    std::size_t added = 0;
    for (const harvest::Discovered& d : found) {
        by_jurisdiction[d.jurisdiction] += 1;
        if (existing.count(d.url) != 0) continue;
        if (add) {
            store.add_source(d.name, d.url, d.jurisdiction);
            ++added;
        }
    }
    std::vector<std::vector<std::string>> rows;
    std::size_t shown = 0;
    for (const auto& [jurisdiction, count] : by_jurisdiction) {
        if (++shown > 20) break;
        rows.push_back({jurisdiction, std::to_string(count)});
    }
    render::table({"jurisdiction", "datasets"}, rows);
    std::printf("  %zu datasets across %zu jurisdictions%s\n", found.size(),
                by_jurisdiction.size(),
                by_jurisdiction.size() > 20 ? " (first 20 shown)" : "");
    if (add) {
        std::printf("  %s %zu new sources added; 'run all' ingests them\n",
                    stamp("ok").c_str(), added);
    } else {
        std::printf("  %s\n", paint("dim", "'catalog add' adds these as sources").c_str());
    }
    return 0;
}

int bench(store::Store& store, pipeline::Pipeline& pipeline, const std::string& golden_path) {
    const std::vector<bench::Golden> golden = bench::load_golden(golden_path);
    for (const bench::Golden& g : golden) {
        for (const auto& [field, labels] : g.fields) {
            if (pipeline.registry().find(field) == nullptr) {
                std::printf("  %s answer key names unknown field '%s' in %s\n",
                            stamp("failed").c_str(), field.c_str(), g.source_id.c_str());
                return 1;
            }
        }
    }
    std::size_t skipped = 0;

    section("Engine vs answer key (" + std::to_string(golden.size()) + " hand-verified sources)");
    BenchTotals engine_totals;
    std::vector<std::vector<std::string>> rows;
    for (const bench::Golden& g : golden) {
        const std::optional<store::CachedFetch> cached = store.fetch_cache(g.source_id);
        if (!cached.has_value()) {
            rows.push_back({g.source_id, stamp("failed"), "no cached bytes: run it first",
                            "", "", ""});
            ++skipped;
            continue;
        }
        const std::optional<store::Source> source = store.find_source(g.source_id);
        const doc::Model model = doc::build_auto(cached->content_type, cached->body);
        const std::string url = source.has_value() ? source->url : "";

        const Stopwatch watch;
        const classify::Prediction prediction = pipeline.classifier().classify(model, url);
        const schema::Mapping mapping =
            schema::infer_mapping(pipeline.registry(), model, pipeline.column_model());
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
    }
    render::table({"source", "class", "predicted", "ok/spur/miss", "mapping F1", "compute"},
                  rows);
    if (engine_totals.docs == 0) {
        std::printf("  %s nothing scored: no golden source has cached bytes (run all first)\n",
                    stamp("failed").c_str());
        return 1;
    }
    print_totals("engine", engine_totals, "$0.00");
    std::printf("  %s\n",
                paint("dim", "the key is hand-verified against the raw columns; misses here "
                             "are real engine mistakes").c_str());
    if (skipped > 0) {
        std::printf("  %s incomplete: %zu of %zu sources had no cached bytes\n",
                    stamp("failed").c_str(), skipped, golden.size());
        return 1;
    }
    return 0;
}
} // namespace dd::cli
