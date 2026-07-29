// The WASM boundary of the engine. The host (a Cloudflare Worker) owns the
// network and the database; this module owns everything the repository is
// about: parsing, classification, schema matching, extraction, event
// building and county compilation. One seam, JSON strings both ways, so the
// host language never touches engine memory layouts.
//
// Contract: every function returns a malloc'd NUL-terminated JSON string the
// caller must release with dd_free. Success is {"ok":true,...}; failure is
// {"ok":false,"error":"..."} and never a fabricated result.

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"
#include "dd/engine/compile.hpp"
#include "dd/engine/events.hpp"
#include "dd/engine/schema.hpp"
#include "dd/ml/classify.hpp"
#include "dd/ml/columns.hpp"
#include "dd/parse/document.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

char* to_c(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out != nullptr) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

char* failure(const std::string& message) {
    dd::json::Writer w;
    w.begin_object();
    w.field("ok", false);
    w.field("error", message);
    w.end_object();
    return to_c(w.take());
}

std::string require(const char* value, const char* name) {
    if (value == nullptr) throw dd::Error(std::string{"missing argument: "} + name);
    return std::string{value};
}

void write_events(dd::json::Writer& w, const std::vector<dd::events::PropertyEvent>& events) {
    w.begin_array();
    for (const dd::events::PropertyEvent& e : events) w.raw_value(e.serialize());
    w.end_array();
}

std::vector<dd::events::PropertyEvent> read_events(const std::string& text) {
    std::vector<dd::events::PropertyEvent> out;
    const dd::json::Value root = dd::json::parse(text);
    if (!root.is_array()) throw dd::Error("events must be a JSON array");
    for (const dd::json::Value& entry : root.items()) {
        out.push_back(dd::events::PropertyEvent::deserialize(entry.serialize()));
    }
    return out;
}

} // namespace

extern "C" {

void dd_free(char* p) { std::free(p); }

char* dd_version() {
    dd::json::Writer w;
    w.begin_object();
    w.field("ok", true);
    w.field("engine", "datadiver");
    w.field("abi", 1);
    w.end_object();
    return to_c(w.take());
}

// One document through the full pipeline: parse, classify, match, extract,
// build events. The host passes the bytes it fetched plus the configuration
// the engine would otherwise read from disk.
char* dd_process_document(const char* schema_json, const char* classifier_json,
                          const char* column_model_json, const char* content_type,
                          const char* body, const char* source_id, const char* jurisdiction,
                          const char* as_of, const char* run_id, const char* now_iso) {
    try {
        const dd::schema::Registry registry =
            dd::schema::Registry::from_json(require(schema_json, "schema_json"));
        const dd::classify::Classifier classifier =
            dd::classify::Classifier::from_json(require(classifier_json, "classifier_json"));
        dd::columns::ColumnModel columns;
        if (column_model_json != nullptr && column_model_json[0] != '\0') {
            columns = dd::columns::ColumnModel::deserialize(column_model_json);
        }

        const dd::Stopwatch parse_watch;
        const dd::doc::Model model = dd::doc::build_auto(require(content_type, "content_type"),
                                                         require(body, "body"));
        const double parse_ms = parse_watch.elapsed_ms();

        const dd::Stopwatch classify_watch;
        const dd::classify::Prediction prediction = classifier.classify(model, "");
        const double classify_ms = classify_watch.elapsed_ms();

        const dd::Stopwatch map_watch;
        const dd::schema::Mapping mapping = dd::schema::infer_mapping(
            registry, model, columns.trained() ? &columns : nullptr);
        const dd::schema::ExtractionResult extraction =
            dd::schema::apply_mapping(registry, mapping, model);
        const double map_ms = map_watch.elapsed_ms();

        const dd::events::EventContext context{
            require(jurisdiction, "jurisdiction"), require(source_id, "source_id"),
            require(as_of, "as_of"), require(run_id, "run_id"), require(now_iso, "now_iso")};
        const std::vector<dd::events::PropertyEvent> events = dd::events::build_events(
            registry, context, prediction.label, prediction.confidence, extraction);

        std::string newest_record_date;
        for (const dd::events::PropertyEvent& e : events) {
            if (e.event_date > newest_record_date) newest_record_date = e.event_date;
        }

        dd::json::Writer w;
        w.begin_object();
        w.field("ok", true);
        w.field("format", std::string{dd::doc::format_name(model.format)});
        w.field("fingerprint", model.structure_fingerprint());
        w.field("classification", prediction.label);
        w.field("class_confidence", prediction.confidence);
        w.field_raw("mapping", mapping.serialize());
        w.field("records", static_cast<std::int64_t>(extraction.records.size()));
        w.field("extraction_rate", extraction.rate);
        w.field("newest_record_date", newest_record_date);
        w.field("parse_ms", parse_ms);
        w.field("classify_ms", classify_ms);
        w.field("map_ms", map_ms);
        w.key("events");
        write_events(w, events);
        w.end_object();
        return to_c(w.take());
    } catch (const std::exception& e) {
        return failure(e.what());
    }
}

// Compiles one county from events the host loaded out of its database.
// trust_json maps source id to {field: mapping confidence}.
char* dd_compile_county(const char* schema_json, const char* events_json,
                        const char* trust_json, const char* county) {
    try {
        const dd::schema::Registry registry =
            dd::schema::Registry::from_json(require(schema_json, "schema_json"));
        const std::vector<dd::events::PropertyEvent> events =
            read_events(require(events_json, "events_json"));

        std::map<std::string, std::vector<dd::events::PropertyEvent>> by_key;
        for (const dd::events::PropertyEvent& e : events) {
            by_key[e.property_key].push_back(e);
        }

        std::map<std::string, std::map<std::string, double>> trust;
        const dd::json::Value parsed = dd::json::parse(require(trust_json, "trust_json"));
        for (const auto& [source, fields] : parsed.members()) {
            for (const auto& [field, confidence] : fields.members()) {
                trust[source][field] = confidence.as_number();
            }
        }

        const std::vector<dd::compile::Property> properties =
            dd::compile::county_from_events(registry, by_key, trust,
                                            require(county, "county"));
        return to_c(dd::compile::render_county_json(county, properties));
    } catch (const std::exception& e) {
        return failure(e.what());
    }
}

} // extern "C"
