#pragma once

#include "dd/engine/events.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <map>
#include <string>
#include <vector>

// Compilation: county store keys merged into deduplicated properties.
// Different government offices key the same parcel differently (treasurer
// account vs assessor parcel), so properties whose normalized addresses
// match are merged, their events pooled chronologically, and each canonical
// field resolved to one value. Conflicts are decided by measured trust: the
// per-field mapping confidence the source earned from the matcher (lexicon,
// transformer verdict, validator pass rate), recency breaking ties - never
// a hand-picked winner. Losing values are kept as recorded conflicts.
namespace dd::compile {

struct ResolvedField {
    std::string value;
    std::string source_id;
    std::string event_date;
    std::string as_of;        // edition of the source the value came from
    double confidence = 0.0;  // the source's measured mapping confidence
};

// One field observed in one edition, kept so a value can be compared with
// what an earlier edition said about the same property.
struct Observation {
    std::string as_of;
    std::string value;
    std::string source_id;
};

struct Conflict {
    std::string field;
    std::string kept_value, kept_source;
    std::string dropped_value, dropped_source;
    double kept_confidence = 0.0;
    double dropped_confidence = 0.0;
};

struct Property {
    std::vector<std::string> keys;  // the store keys merged into this record
    // A record locates a building when it carries a parcel id or a street
    // address with a real house number. Complaint feeds also publish block
    // references ("3200 BLOCK OF ARGONNE AVENUE") and placeholder addresses,
    // which name an area rather than a property and can never be enriched.
    bool locates_a_building = false;
    std::vector<events::PropertyEvent> events;  // chronological
    events::State state = events::State::Normal;
    std::map<std::string, ResolvedField> fields;
    // Every distinct edition that reported a field, newest first. A single
    // roll leaves one entry; successive rolls make year over year deltas
    // computable without losing the current value.
    std::map<std::string, std::vector<Observation>> history;
    std::vector<Conflict> conflicts;
    // Signals measured from the event history.
    double due = 0.0;
    double assessed = 0.0;
    double assessed_previous = 0.0;  // the same field one edition earlier
    std::size_t violations = 0;
    std::string auction_date;
};

// Per (source, field) trust: the confidence of that field's mapping in the
// source's learned state. Sources without a stored mapping fall back to the
// event's classifier confidence at resolution time.
std::map<std::string, std::map<std::string, double>> source_trust(store::Store& store);

// All properties for a county, merged and resolved, most distressed first
// (owed desc, then violations, then assessed value).
std::vector<Property> county(store::Store& store, const schema::Registry& registry,
                             const std::string& county);

// The same compilation as a pure function over already-loaded events, for
// hosts that keep events somewhere other than the file store (the WASM
// build's events live in the caller's database). Keys are store property
// keys; trust maps source id to per-field mapping confidence.
std::vector<Property> county_from_events(
    const schema::Registry& registry,
    const std::map<std::string, std::vector<events::PropertyEvent>>& events_by_key,
    const std::map<std::string, std::map<std::string, double>>& trust,
    const std::string& county);

// Renders compiled properties as the canonical county payload an API serves.
std::string render_county_json(const std::string& county,
                               const std::vector<Property>& properties);

} // namespace dd::compile
