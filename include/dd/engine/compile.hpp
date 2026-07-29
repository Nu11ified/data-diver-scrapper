#pragma once

#include "dd/engine/events.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dd::compile {
struct ResolvedField {
    std::string value;
    std::string source_id;
    std::string event_date;
    std::string as_of;        // edition of the source the value came from
    double confidence = 0.0;  // the source's measured mapping confidence
};

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
    bool locates_a_building = false;
    std::vector<events::PropertyEvent> events;  // chronological
    events::State state = events::State::Normal;
    std::map<std::string, ResolvedField> fields;
    std::map<std::string, std::vector<Observation>> history;
    std::vector<Conflict> conflicts;
    double due = 0.0;
    double assessed = 0.0;
    double assessed_previous = 0.0;  // the same field one edition earlier
    std::size_t violations = 0;
    std::string auction_date;
    std::optional<std::int64_t> days_since_event;  // empty when no event carries a usable date
};

std::map<std::string, std::map<std::string, double>> source_trust(store::Store& store);

std::vector<Property> county(store::Store& store, const schema::Registry& registry,
                             const std::string& county);

std::vector<Property> county_from_events(
    const schema::Registry& registry,
    const std::map<std::string, std::vector<events::PropertyEvent>>& events_by_key,
    const std::map<std::string, std::map<std::string, double>>& trust,
    const std::string& county);

std::string render_county_json(const std::string& county,
                               const std::vector<Property>& properties);
} // namespace dd::compile
