#include "dd/engine/pipeline.hpp"

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

std::string pick(const std::map<std::string, std::string>& values, const char* key) {
    const auto it = values.find(key);
    return it == values.end() ? std::string{} : it->second;
}

// Builds property events from the canonical records of one run.
std::vector<events::PropertyEvent> to_events(const store::Source& source,
                                             const store::RunRecord& run,
                                             const std::string& classification,
                                             double class_confidence,
                                             const schema::ExtractionResult& extraction) {
    std::vector<events::PropertyEvent> out;
    for (const schema::CanonicalRecord& record : extraction.records) {
        const std::string parcel = pick(record.values, "parcel_id");
        const std::string address = pick(record.values, "address");
        const std::string key = entity::property_key(source.jurisdiction, parcel, address);
        if (key.empty()) continue; // unresolvable: no identity evidence

        events::PropertyEvent e;
        e.property_key = key;
        e.kind = events::kind_from_source_label(classification, pick(record.values, "status"));
        e.event_date = pick(record.values, "event_date");
        if (e.event_date.empty()) e.event_date = pick(record.values, "auction_date");
        e.recorded_at = run.started_at;
        e.source_id = source.id;
        e.run_id = run.id;
        e.confidence = class_confidence;
        for (const char* money_field : {"amount_due", "sale_price", "assessed_value"}) {
            const std::string amount = pick(record.values, money_field);
            if (!amount.empty()) {
                e.amount = std::atof(amount.c_str());
                break;
            }
        }
        e.details = record.values;
        e.id = events::PropertyEvent::compute_id(e);
        out.push_back(std::move(e));
    }
    return out;
}

// The full extraction picture for one source, kept for the schema view: the
// dialect's own labels, the classifier's posterior, the mapping with its per
// field evidence, the measured per-field rates, and the records themselves.
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

// Exponential moving average keeps the baseline honest about gradual change
// while still reacting to a collapse.
double update_baseline(double baseline, int good_runs, double rate) {
    if (good_runs <= 0) return rate;
    constexpr double kAlpha = 0.3;
    return baseline * (1.0 - kAlpha) + rate * kAlpha;
}

} // namespace

Pipeline::Pipeline(store::Store& store, classify::Classifier classifier)
    : store_{store}, classifier_{std::move(classifier)} {}

store::RunRecord Pipeline::run_source_id(const std::string& source_id) {
    const std::optional<store::Source> source = store_.find_source(source_id);
    if (!source.has_value()) throw Error("pipeline: unknown source: " + source_id);
    return run_source(*source);
}

store::RunRecord Pipeline::run_source(const store::Source& source) {
    const Stopwatch total_watch;
    const double cpu_before = metrics::cpu_time_ms();

    store::RunRecord run;
    run.id = new_run_id(source.id);
    run.source_id = source.id;
    run.started_at = timeutil::iso_now();

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

    // ------------------------------------------------------------ fetch ----
    const fetch::Result fetched = fetch::get(source.url);
    run.http_status = fetched.http_status;
    run.bytes = fetched.bytes;
    run.fetch_ms = fetched.total_ms;
    if (!fetched.ok) return finish(false, "fetch", fetched.error);

    // ------------------------------------------------- detect and extract --
    const Stopwatch parse_watch;
    doc::Model model;
    try {
        model = doc::build_auto(fetched.content_type, fetched.body);
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

    // --------------------------------------------------------- classify ----
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

    // ------------------------------------------------- map, detect drift ---
    const Stopwatch map_watch;
    store::SourceState state = store_.source_state(source.id);
    run.baseline_rate = state.baseline_rate;

    schema::Mapping mapping;
    schema::ExtractionResult extraction;

    if (state.has_mapping) {
        extraction = schema::apply_mapping(state.mapping, model);
        const heal::Assessment verdict = heal::assess(state, model, extraction);
        run.drift_detected = verdict.drift;
        if (verdict.drift) {
            run.repair_attempted = true;
            const heal::Proposal proposal = heal::propose(model, state.mapping, state.baseline_rate);

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
            repair.changes = proposal.changes;
            store_.add_repair(repair);

            if (proposal.acceptable) {
                run.repair_accepted = true;
                mapping = proposal.candidate;
                extraction = proposal.result;
                state.mapping = mapping;
                state.has_mapping = true;
                // The source changed shape: the old baseline no longer
                // describes this structure. Restart it from the repair.
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
        mapping = schema::infer_mapping(model);
        if (mapping.fields.empty()) {
            run.map_ms = map_watch.elapsed_ms();
            return finish(false, "map",
                          "could not learn a mapping with an identity field from this document");
        }
        extraction = schema::apply_mapping(mapping, model);
        state.mapping = mapping;
        state.has_mapping = true;
        state.good_runs = 0;
        logging::info("pipeline: learned initial mapping for " + source.id);
    }
    run.map_ms = map_watch.elapsed_ms();
    run.extraction_rate = extraction.rate;
    run.mapping_confidence = mapping.confidence;

    // --------------------------------------------- resolve and add events --
    const std::vector<events::PropertyEvent> batch =
        to_events(source, run, prediction.label, prediction.confidence, extraction);
    run.events_new = static_cast<std::int64_t>(store_.add_events(batch));
    store_.save_latest_records(source.id,
                               extraction_snapshot(run, model, prediction, mapping, extraction));

    // ------------------------------------------------------ update state ---
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
