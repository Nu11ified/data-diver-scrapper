#include "dd/engine/pipeline.hpp"

#include <atomic>
#include <set>
#include <thread>

#include "dd/core/core.hpp"
#include "dd/parse/document.hpp"
#include "dd/engine/entity.hpp"
#include "dd/engine/events.hpp"
#include "dd/net/fetch.hpp"
#include "dd/engine/heal.hpp"
#include "dd/core/json.hpp"
#include "dd/core/metrics.hpp"

#include <utility>

namespace dd::pipeline {
namespace {
std::string new_run_id(const std::string& source_id) {
    return str::hex64(str::hash64(source_id + "|" + timeutil::iso_now() + "|" +
                                  std::to_string(metrics::cpu_time_ms())));
}

std::vector<events::PropertyEvent> to_events(const schema::Registry& registry,
                                             const store::Source& source,
                                             const store::RunRecord& run,
                                             const std::string& classification,
                                             double class_confidence,
                                             const schema::ExtractionResult& extraction) {
    return events::build_events(
        registry,
        events::EventContext{source.jurisdiction, source.id, source.as_of, run.id,
                             run.started_at},
        classification, class_confidence, extraction);
}

std::string extraction_snapshot(const store::RunRecord& run, const doc::Model& model,
                                const classify::Prediction& prediction,
                                const schema::Mapping& mapping,
                                const schema::ExtractionResult& extraction) {
    json::Writer w;
    w.begin_object();
    w.field("at", run.started_at);
    w.field("run_id", run.id);
    w.field("format", std::string{doc::format_name(model.format)});
    w.field("container", model.container_signature);
    w.field("fingerprint", model.structure_fingerprint());
    w.key("labels");
    w.begin_array();
    for (const std::string& label : model.labels) w.string_value(label);
    w.end_array();
    w.key("raw_sample");
    w.begin_array();
    if (!model.records.empty()) {
        std::size_t emitted = 0;
        for (const doc::Cell& cell : model.records.front().cells) {
            if (++emitted > 12) break;
            w.begin_object();
            w.field("label", cell.label);
            w.field("value", cell.value);
            w.end_object();
        }
    }
    w.end_array();
    w.key("classification");
    w.begin_object();
    w.field("label", prediction.label);
    w.field("confidence", prediction.confidence);
    w.key("distribution");
    w.begin_array();
    for (const model::Scored& s : prediction.distribution) {
        w.begin_object();
        w.field("label", s.label);
        w.field("probability", s.probability);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    w.field_raw("mapping", mapping.serialize());
    w.key("field_rates");
    w.begin_object();
    for (const auto& [field, rate] : extraction.field_rates) w.field(field, rate);
    w.end_object();
    w.key("records");
    w.begin_array();
    for (const schema::CanonicalRecord& record : extraction.records) {
        w.begin_object();
        for (const auto& [field, value] : record.values) w.field(field, value);
        w.field("_completeness", record.completeness);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.take();
}

double update_baseline(double baseline, int good_runs, double rate) {
    if (good_runs <= 0) return rate;
    constexpr double kAlpha = 0.3;
    return baseline * (1.0 - kAlpha) + rate * kAlpha;
}
} // namespace

Pipeline::Pipeline(store::Store& store, classify::Classifier classifier,
                   schema::Registry registry)
    : store_{store}, classifier_{std::move(classifier)}, registry_{std::move(registry)} {}

void Pipeline::set_column_model(columns::ColumnModel model) {
    column_model_ = std::move(model);
}

void Pipeline::set_classifier(classify::Classifier classifier) {
    classifier_ = std::move(classifier);
}

store::RunRecord Pipeline::run_source_id(const std::string& source_id) {
    const std::optional<store::Source> source = store_.find_source(source_id);
    if (!source.has_value()) throw Error("pipeline: unknown source: " + source_id);
    return run_source(*source);
}

namespace {
bool quote_safe(const std::string& value) {
    if (value.empty()) return false;
    for (char c : value) {
        if (c == '\'' || c == '"' || c == '\n') return false;
    }
    return true;
}

std::vector<std::string> tracked_parcels(const store::Source& source, const store::Store& store) {
    const std::string prefix = str::slug(source.jurisdiction) + "|p:";
    std::vector<std::string> out;
    for (const std::string& key : store.property_keys()) {
        if (out.size() >= 150) break;
        if (key.rfind(prefix, 0) != 0) continue;
        const std::string parcel = key.substr(prefix.size());
        if (quote_safe(parcel)) out.push_back("'" + parcel + "'");
    }
    return out;
}

std::vector<std::string> tracked_streets(const store::Source& source, store::Store& store) {
    static const std::vector<std::string> kSuffixes = {
        "ST", "AVE", "AV", "RD", "DR", "BLVD", "LN", "CT", "PL", "CIR", "TER", "WAY",
        "PKWY", "HWY", "TRL", "SQ", "STREET", "AVENUE", "ROAD", "DRIVE", "COURT",
        "BOULEVARD", "LANE", "PLACE", "CRESCENT", "N", "S", "E", "W"};
    const std::string prefix = str::slug(source.jurisdiction) + "|";
    std::vector<std::string> out;
    for (const std::string& key : store.property_keys()) {
        if (out.size() >= 60) break;
        if (key.rfind(prefix, 0) != 0) continue;
        for (const events::PropertyEvent& e : store.events_for(key)) {
            const auto it = e.details.find("address");
            if (it == e.details.end()) continue;
            std::vector<std::string> words = str::split(str::to_upper(it->second), ' ');
            std::vector<std::string> name;
            for (const std::string& word : words) {
                if (word.empty() || str::is_digits(word)) continue;
                if (std::find(kSuffixes.begin(), kSuffixes.end(), word) != kSuffixes.end()) {
                    continue;
                }
                name.push_back(word);
            }
            const std::string street = str::join(name, " ");
            if (!quote_safe(street) || street.size() < 3) continue;
            const std::string quoted = "'" + street + "'";
            if (std::find(out.begin(), out.end(), quoted) == out.end()) out.push_back(quoted);
            break;
        }
    }
    return out;
}

std::vector<std::string> tracked_numbers(const store::Source& source, store::Store& store) {
    const std::string prefix = str::slug(source.jurisdiction) + "|";
    std::vector<std::string> out;
    for (const std::string& key : store.property_keys()) {
        if (out.size() >= 120) break;
        if (key.rfind(prefix, 0) != 0) continue;
        for (const events::PropertyEvent& e : store.events_for(key)) {
            const auto it = e.details.find("address");
            if (it == e.details.end()) continue;
            const entity::Address address = entity::parse_address(it->second);
            if (!address.locatable) break;
            const std::string quoted = "'" + address.number + "'";
            if (std::find(out.begin(), out.end(), quoted) == out.end()) out.push_back(quoted);
            break;
        }
    }
    return out;
}
} // namespace

std::string order_by_learned_date(std::string url, const schema::Registry& registry,
                                  const store::SourceState& state) {
    if (!state.has_mapping || !str::contains(url, "/resource/")) return url;
    if (str::contains(url, "$order")) return url;
    const std::vector<const schema::FieldDef*> dated = registry.with_role("event_date");
    for (const schema::FieldDef* field : dated) {
        const schema::FieldMapping* mapped = state.mapping.find(field->name);
        if (mapped == nullptr || mapped->source_label.empty()) continue;
        if (str::contains(mapped->source_label, " ")) continue; // not a query column
        return url + (str::contains(url, "?") ? "&" : "?") + "$order=" +
               mapped->source_label + " DESC";
    }
    return url;
}

std::string expand_url_template(const store::Source& source, store::Store& store) {
    std::string url = source.url;
    const auto substitute = [&](const std::string& token,
                                const std::vector<std::string>& values) {
        const std::size_t at = url.find(token);
        if (at == std::string::npos) return;
        if (values.empty()) {
            throw Error("enrichment source " + source.id + " has nothing to target yet; run " +
                        "the jurisdiction's primary sources first");
        }
        url = url.substr(0, at) + str::join(values, ",") + url.substr(at + token.size());
    };
    substitute("{parcels}", tracked_parcels(source, store));
    substitute("{streets}", tracked_streets(source, store));
    substitute("{numbers}", tracked_numbers(source, store));
    return url;
}

std::vector<store::RunRecord> Pipeline::run_sources(const std::vector<store::Source>& sources,
                                                    int threads) {
    std::vector<store::RunRecord> out(sources.size());
    if (sources.empty()) return out;

    std::vector<std::size_t> primary;
    std::vector<std::size_t> enrichment;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        (str::contains(sources[i].url, "{parcels}") ? enrichment : primary).push_back(i);
    }

    const int hardware = static_cast<int>(std::thread::hardware_concurrency());
    const auto run_wave = [&](const std::vector<std::size_t>& wave) {
        if (wave.empty()) return;
        const int workers = std::min<int>(
            static_cast<int>(wave.size()),
            threads > 0 ? threads : std::max(2, std::min(8, hardware)));
        if (workers <= 1) {
            for (const std::size_t i : wave) out[i] = run_source(sources[i]);
            return;
        }
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(workers));
        for (int w = 0; w < workers; ++w) {
            pool.emplace_back([&] {
                for (std::size_t at = next++; at < wave.size(); at = next++) {
                    out[wave[at]] = run_source(sources[wave[at]]);
                }
            });
        }
        for (std::thread& t : pool) t.join();
    };
    run_wave(primary);
    run_wave(enrichment);
    return out;
}

store::RunRecord Pipeline::run_source(const store::Source& source) {
    const Stopwatch total_watch;
    const double cpu_before = metrics::cpu_time_ms();

    store::RunRecord run;
    run.id = new_run_id(source.id);
    run.source_id = source.id;
    run.started_at = timeutil::iso_now();

    std::string url;
    try {
        url = order_by_learned_date(expand_url_template(source, store_), registry_,
                                    store_.source_state(source.id));
    } catch (const Error& e) {
        run.ok = false;
        run.stage = "fetch";
        run.error = e.what();
        run.total_ms = total_watch.elapsed_ms();
        run.cpu_ms = metrics::cpu_time_ms() - cpu_before;
        run.rss_bytes = metrics::current_rss_bytes();
        store_.record_run(run);
        return run;
    }
    const fetch::Result fetched = fetch::get(url);
    run.http_status = fetched.http_status;
    run.bytes = fetched.bytes;
    run.fetch_ms = fetched.total_ms;
    if (!fetched.ok) {
        run.ok = false;
        run.stage = "fetch";
        run.error = fetched.error;
        run.total_ms = total_watch.elapsed_ms();
        run.cpu_ms = metrics::cpu_time_ms() - cpu_before;
        run.rss_bytes = metrics::current_rss_bytes();
        store_.record_run(run);
        return run;
    }
    store_.save_fetch_cache(source.id, fetched.content_type, fetched.body);

    return ingest(source, std::move(run), total_watch, cpu_before, fetched.content_type,
                  fetched.body);
}

store::RunRecord Pipeline::run_cached(const store::Source& source) {
    const Stopwatch total_watch;
    const double cpu_before = metrics::cpu_time_ms();

    store::RunRecord run;
    run.id = new_run_id(source.id);
    run.source_id = source.id;
    run.started_at = timeutil::iso_now();

    const Stopwatch cache_watch;
    const std::optional<store::CachedFetch> cached = store_.fetch_cache(source.id);
    if (!cached.has_value()) throw Error("pipeline: no fetch cache for " + source.id);
    run.fetch_ms = cache_watch.elapsed_ms();
    run.bytes = static_cast<std::int64_t>(cached->body.size());

    return ingest(source, std::move(run), total_watch, cpu_before, cached->content_type,
                  cached->body);
}

store::RunRecord Pipeline::ingest(const store::Source& source, store::RunRecord run,
                                  const Stopwatch& total_watch, double cpu_before,
                                  const std::string& content_type, const std::string& body) {
    auto finish = [&](bool ok, const std::string& stage, const std::string& error) {
        run.ok = ok;
        run.stage = stage;
        run.error = error;
        run.total_ms = total_watch.elapsed_ms();
        run.cpu_ms = metrics::cpu_time_ms() - cpu_before;
        run.rss_bytes = metrics::current_rss_bytes();
        store_.record_run(run);
        return run;
    };

    const Stopwatch parse_watch;
    doc::Model model;
    try {
        model = doc::build_auto(content_type, body);
    } catch (const Error& e) {
        run.parse_ms = parse_watch.elapsed_ms();
        return finish(false, "parse", e.what());
    }
    run.parse_ms = parse_watch.elapsed_ms();
    run.format = std::string{doc::format_name(model.format)};
    run.structure_fingerprint = model.structure_fingerprint();
    run.records_extracted = static_cast<std::int64_t>(model.records.size());
    if (model.records.empty() && model.text.empty()) {
        return finish(false, "parse", "document yielded no text and no records");
    }

    const Stopwatch classify_watch;
    classify::Prediction prediction;
    try {
        prediction = classifier_.classify(model, source.url);
    } catch (const Error& e) {
        run.classify_ms = classify_watch.elapsed_ms();
        return finish(false, "classify", e.what());
    }
    run.classify_ms = classify_watch.elapsed_ms();
    run.classification = prediction.label;
    run.class_confidence = prediction.confidence;

    if (model.records.empty()) {
        return finish(false, "map",
                      "no records extracted from " + run.format + " document");
    }

    const Stopwatch map_watch;
    store::SourceState state = store_.source_state(source.id);
    run.baseline_rate = state.baseline_rate;

    schema::Mapping mapping;
    schema::ExtractionResult extraction;

    if (state.has_mapping) {
        extraction = schema::apply_mapping(registry_, state.mapping, model);
        const heal::Assessment verdict = heal::assess(state, model, extraction);
        run.drift_detected = verdict.drift;
        if (verdict.drift) {
            run.repair_attempted = true;
            const heal::Proposal proposal =
                heal::propose(registry_, model, state.mapping, state.baseline_rate, column_model());

            store::RepairRecord repair;
            repair.id = new_run_id(source.id + "|repair");
            repair.source_id = source.id;
            repair.at = run.started_at;
            repair.reason = verdict.reason;
            repair.before_mapping_json = state.mapping.serialize();
            repair.after_mapping_json = proposal.candidate.serialize();
            repair.before_rate = extraction.rate;
            repair.after_rate = proposal.result.rate;
            repair.confidence = proposal.confidence;
            repair.accepted = proposal.acceptable;
            repair.resolution = proposal.acceptable ? "auto" : "pending";
            repair.changes = proposal.changes;
            store_.add_repair(repair);

            if (proposal.acceptable) {
                run.repair_accepted = true;
                mapping = proposal.candidate;
                state.good_runs = 0;
                logging::info("pipeline: accepted mapping repair for " + source.id);
            } else {
                run.map_ms = map_watch.elapsed_ms();
                run.extraction_rate = extraction.rate;
                run.mapping_confidence = state.mapping.confidence;
                return finish(false, "map",
                              "drift detected and repair below auto-accept bar; queued for review");
            }
        } else {
            mapping = state.mapping;
        }
    } else {
        mapping = schema::infer_mapping(registry_, model, column_model());
        if (!mapping.fields.empty()) {
            state.good_runs = 0;
            logging::info("pipeline: learned initial mapping for " + source.id);
        }
    }

    if (!state.overrides.empty()) {
        mapping = schema::apply_overrides(registry_, mapping, state.overrides, model);
    }
    if (mapping.fields.empty()) {
        run.map_ms = map_watch.elapsed_ms();
        return finish(false, "map",
                      "could not learn a mapping with an identity field from this document");
    }
    extraction = schema::apply_mapping(registry_, mapping, model);
    state.mapping = mapping;
    state.has_mapping = true;
    run.map_ms = map_watch.elapsed_ms();
    run.extraction_rate = extraction.rate;
    run.mapping_confidence = mapping.confidence;

    std::vector<events::PropertyEvent> batch =
        to_events(registry_, source, run, prediction.label, prediction.confidence, extraction);
    if (str::contains(source.url, "{parcels}") || str::contains(source.url, "{streets}")) {
        const std::size_t before = batch.size();
        std::set<std::string> known_keys;
        std::set<std::string> known_addresses;
        const std::string prefix = str::slug(source.jurisdiction) + "|";
        for (const std::string& key : store_.property_keys()) {
            if (key.rfind(prefix, 0) != 0) continue;
            known_keys.insert(key);
            for (const events::PropertyEvent& e : store_.events_for(key)) {
                const auto it = e.details.find("address");
                if (it == e.details.end() || it->second.empty()) continue;
                const std::string join =
                    entity::address_join_key(entity::parse_address(it->second));
                if (!join.empty()) known_addresses.insert(join);
            }
        }
        std::erase_if(batch, [&](const events::PropertyEvent& e) {
            if (known_keys.count(e.property_key) != 0) return false;
            const auto it = e.details.find("address");
            if (it == e.details.end()) return true;
            const std::string join =
                entity::address_join_key(entity::parse_address(it->second));
            return join.empty() || known_addresses.count(join) == 0;
        });
        if (batch.size() < before) {
            logging::info("pipeline: " + source.id + " kept " + std::to_string(batch.size()) +
                          " of " + std::to_string(before) + " records that match tracked "
                          "properties");
        }
    }
    for (const events::PropertyEvent& e : batch) {
        if (e.event_date > run.newest_record_date) run.newest_record_date = e.event_date;
    }
    run.events_new = static_cast<std::int64_t>(store_.add_events(batch));
    store_.save_latest_records(source.id,
                               extraction_snapshot(run, model, prediction, mapping, extraction));

    state.source_id = source.id;
    state.baseline_rate = update_baseline(state.baseline_rate, state.good_runs, extraction.rate);
    state.good_runs += 1;
    state.fingerprint = model.structure_fingerprint();
    state.classification = prediction.label;
    state.updated_at = timeutil::iso_now();
    store_.save_source_state(state);

    return finish(true, "done", "");
}
} // namespace dd::pipeline
