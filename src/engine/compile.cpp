#include "dd/engine/compile.hpp"

#include "dd/core/core.hpp"
#include "dd/engine/entity.hpp"

#include <algorithm>

namespace dd::compile {
namespace {

void sort_events(std::vector<events::PropertyEvent>* evs) {
    std::stable_sort(evs->begin(), evs->end(),
                     [](const events::PropertyEvent& a, const events::PropertyEvent& b) {
                         if (a.event_date != b.event_date) return a.event_date < b.event_date;
                         if (a.recorded_at != b.recorded_at) return a.recorded_at < b.recorded_at;
                         return a.id < b.id;
                     });
}

std::string first_address(const std::vector<events::PropertyEvent>& evs) {
    for (const events::PropertyEvent& e : evs) {
        const auto it = e.details.find("address");
        if (it != e.details.end() && !it->second.empty()) return it->second;
    }
    return {};
}

// An address pins down one building when it starts with a real house number
// and does not name a block. "0 ADMIRAL TAUSSIG BLVD" is a placeholder shared
// by many parcels, "3200 BLOCK OF ARGONNE AVE" is a complaint location, and
// "S S 50TH ST" names a stretch of street.
bool mergeable_address(const std::string& normalized) {
    if (str::contains(normalized, "block of") || str::contains(normalized, "blk of")) {
        return false;
    }
    const std::size_t space = normalized.find(' ');
    if (space == std::string::npos || space == 0) return false;
    const std::string number = normalized.substr(0, space);
    return str::is_digits(number) && number != "0";
}

// Whether two raw values mean the same thing for this field: compare through
// the field's normalizer so "$110,091" equals "110091.00" and case noise is
// not a conflict; addresses compare through the entity normalizer so street
// dialects ("ST" vs "STREET", padded house numbers) are not conflicts.
bool same_value(const schema::FieldDef& field, const std::string& a, const std::string& b) {
    if (field.kind == schema::Kind::Address) {
        return entity::normalize_address(a) == entity::normalize_address(b);
    }
    return str::to_lower(schema::normalize(field, a)) == str::to_lower(schema::normalize(field, b));
}

void resolve_fields(const schema::Registry& registry,
                    const std::map<std::string, std::map<std::string, double>>& trust,
                    Property* property) {
    for (const schema::FieldDef& field : registry.fields()) {
        ResolvedField best;
        bool found = false;
        for (const events::PropertyEvent& e : property->events) {
            const auto it = e.details.find(field.name);
            if (it == e.details.end() || it->second.empty()) continue;
            double confidence = e.confidence;
            const auto source_it = trust.find(e.source_id);
            if (source_it != trust.end()) {
                const auto field_it = source_it->second.find(field.name);
                if (field_it != source_it->second.end()) confidence = field_it->second;
            }
            const bool wins =
                !found || confidence > best.confidence ||
                (confidence == best.confidence && e.event_date >= best.event_date);
            if (found && !same_value(field, it->second, best.value)) {
                Conflict c;
                c.field = field.name;
                if (wins) {
                    c.kept_value = it->second;
                    c.kept_source = e.source_id;
                    c.kept_confidence = confidence;
                    c.dropped_value = best.value;
                    c.dropped_source = best.source_id;
                    c.dropped_confidence = best.confidence;
                } else {
                    c.kept_value = best.value;
                    c.kept_source = best.source_id;
                    c.kept_confidence = best.confidence;
                    c.dropped_value = it->second;
                    c.dropped_source = e.source_id;
                    c.dropped_confidence = confidence;
                }
                property->conflicts.push_back(std::move(c));
            }
            if (wins) {
                best = ResolvedField{it->second, e.source_id, e.event_date, confidence};
                found = true;
            }
        }
        if (found) property->fields[field.name] = std::move(best);
    }
}

void measure_signals(Property* property) {
    for (const events::PropertyEvent& e : property->events) {
        switch (e.kind) {
        case events::Kind::TaxDelinquency: property->due = e.amount; break;
        case events::Kind::CodeViolation: ++property->violations; break;
        case events::Kind::AuctionScheduled: property->auction_date = e.event_date; break;
        default: break;
        }
    }
    const auto assessed = property->fields.find("assessed_value");
    if (assessed != property->fields.end()) {
        const std::optional<double> parsed = schema::parse_money(assessed->second.value);
        if (parsed.has_value() && *parsed > 0.0) property->assessed = *parsed;
    }
}

} // namespace

std::map<std::string, std::map<std::string, double>> source_trust(store::Store& store) {
    std::map<std::string, std::map<std::string, double>> trust;
    for (const store::Source& source : store.sources()) {
        const store::SourceState state = store.source_state(source.id);
        if (!state.has_mapping) continue;
        for (const schema::FieldMapping& fm : state.mapping.fields) {
            trust[source.id][fm.field] = fm.confidence;
        }
    }
    return trust;
}

std::vector<Property> county(store::Store& store, const schema::Registry& registry,
                             const std::string& county) {
    const std::string wanted = str::slug(county);
    const std::map<std::string, std::map<std::string, double>> trust = source_trust(store);

    // Group store keys: same normalized address in the same county is the
    // same property, whatever each office called it.
    std::map<std::string, std::vector<std::string>> groups;
    std::map<std::string, std::vector<events::PropertyEvent>> events_by_key;
    for (const std::string& key : store.property_keys()) {
        const std::string slug = key.substr(0, key.find('|'));
        if (slug != wanted && !str::contains(slug, wanted)) continue;
        std::vector<events::PropertyEvent> evs = store.events_for(key);
        const std::string address = entity::normalize_address(first_address(evs));
        const std::string group =
            mergeable_address(address) ? slug + "|a:" + address : key;
        groups[group].push_back(key);
        events_by_key[key] = std::move(evs);
    }

    std::vector<Property> out;
    for (auto& [group, keys] : groups) {
        Property property;
        std::sort(keys.begin(), keys.end());
        property.keys = keys;
        for (const std::string& key : keys) {
            std::vector<events::PropertyEvent>& evs = events_by_key[key];
            property.events.insert(property.events.end(), evs.begin(), evs.end());
        }
        sort_events(&property.events);
        property.state = events::reduce(property.events).state;
        resolve_fields(registry, trust, &property);
        measure_signals(&property);
        const auto parcel = property.fields.find("parcel_id");
        const auto address = property.fields.find("address");
        property.locates_a_building =
            (parcel != property.fields.end() && !parcel->second.value.empty()) ||
            (address != property.fields.end() &&
             mergeable_address(entity::normalize_address(address->second.value)));
        out.push_back(std::move(property));
    }

    std::stable_sort(out.begin(), out.end(), [](const Property& a, const Property& b) {
        if (a.due != b.due) return a.due > b.due;
        if (a.violations != b.violations) return a.violations > b.violations;
        return a.assessed > b.assessed;
    });
    return out;
}

} // namespace dd::compile
