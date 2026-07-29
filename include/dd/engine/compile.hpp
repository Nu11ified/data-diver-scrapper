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
    double confidence = 0.0;  // the source's measured mapping confidence
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
    std::vector<events::PropertyEvent> events;  // chronological
    events::State state = events::State::Normal;
    std::map<std::string, ResolvedField> fields;
    std::vector<Conflict> conflicts;
    // Signals measured from the event history.
    double due = 0.0;
    double assessed = 0.0;
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

} // namespace dd::compile
