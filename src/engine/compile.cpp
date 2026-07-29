#include "dd/engine/compile.hpp"

#include "dd/core/core.hpp"
#include "dd/engine/entity.hpp"

#include <algorithm>
#include <set>

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

            // Record what this edition says before deciding the current
            // value, so an older roll stays available for comparison.
            std::vector<Observation>& seen = property->history[field.name];
            const bool already = std::any_of(
                seen.begin(), seen.end(), [&](const Observation& o) {
                    return o.as_of == e.as_of && o.source_id == e.source_id;
                });
            if (!already) seen.push_back(Observation{e.as_of, it->second, e.source_id});

            // Editions of the same dated series are ordered by edition: every
            // row in an assessment roll is equally current, so no per record
            // date can tell this year's roll from last year's. A live feed
            // carries no edition and is never ranked against a roll that way,
            // so those comparisons fall through to measured trust, then
            // recency.
            const bool both_dated = !e.as_of.empty() && !best.as_of.empty();
            bool wins;
            if (!found) {
                wins = true;
            } else if (both_dated && e.as_of != best.as_of) {
                wins = e.as_of > best.as_of;
            } else {
                wins = confidence > best.confidence ||
                       (confidence == best.confidence && e.event_date >= best.event_date);
            }
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
                best = ResolvedField{it->second, e.source_id, e.event_date, e.as_of, confidence};
                found = true;
            }
        }
        if (found) property->fields[field.name] = std::move(best);
    }
    for (auto& [field, seen] : property->history) {
        std::stable_sort(seen.begin(), seen.end(),
                         [](const Observation& a, const Observation& b) {
                             return a.as_of > b.as_of;
                         });
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
    // The same field one edition earlier, which makes the change measurable.
    const auto seen = property->history.find("assessed_value");
    if (seen != property->history.end()) {
        for (const Observation& o : seen->second) {
            if (o.as_of == assessed->second.as_of) continue;
            const std::optional<double> parsed = schema::parse_money(o.value);
            if (parsed.has_value() && *parsed > 0.0) {
                property->assessed_previous = *parsed;
                break;
            }
        }
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

    // Group store keys: one building may appear under a treasurer account, an
    // assessor parcel and a complaint id at once, so keys join on the parts
    // of the address every office agrees about. A group whose members
    // contradict each other on a part they both publish is not merged.
    std::map<std::string, std::vector<std::string>> groups;
    std::map<std::string, std::vector<events::PropertyEvent>> events_by_key;
    std::map<std::string, entity::Address> address_by_key;
    for (const std::string& key : store.property_keys()) {
        const std::string slug = key.substr(0, key.find('|'));
        if (slug != wanted && !str::contains(slug, wanted)) continue;
        std::vector<events::PropertyEvent> evs = store.events_for(key);
        const entity::Address address = entity::parse_address(first_address(evs));
        const std::string join = entity::address_join_key(address);
        groups[join.empty() ? key : slug + "|a:" + join].push_back(key);
        address_by_key[key] = address;
        events_by_key[key] = std::move(evs);
    }

    // An address that resolves to several parcels inside one office is not a
    // unique identifier for a building: an airport or an apartment complex
    // files many parcels at one street address. Those groups do not merge.
    const auto address_is_unique = [&](const std::vector<std::string>& keys) {
        std::map<std::string, std::set<std::string>> parcels_per_source;
        for (const std::string& key : keys) {
            for (const events::PropertyEvent& e : events_by_key[key]) {
                const auto it = e.details.find("parcel_id");
                if (it == e.details.end() || it->second.empty()) continue;
                parcels_per_source[e.source_id].insert(it->second);
            }
        }
        for (const auto& [source, parcels] : parcels_per_source) {
            if (parcels.size() > 1) return false;
        }
        return true;
    };
    std::map<std::string, std::vector<std::string>> unique_groups;
    for (auto& [group, keys] : groups) {
        if (keys.size() == 1 || address_is_unique(keys)) {
            unique_groups[group] = std::move(keys);
            continue;
        }
        for (const std::string& key : keys) unique_groups[key] = {key};
    }
    groups = std::move(unique_groups);

    // Split any group whose addresses contradict: 100 MAIN ST E and
    // 100 MAIN ST W share a join key but are different buildings.
    std::map<std::string, std::vector<std::string>> resolved_groups;
    for (auto& [group, keys] : groups) {
        std::vector<std::vector<std::string>> buckets;
        for (const std::string& key : keys) {
            const entity::Address& address = address_by_key[key];
            bool placed = false;
            for (std::vector<std::string>& bucket : buckets) {
                const bool fits = std::all_of(
                    bucket.begin(), bucket.end(), [&](const std::string& member) {
                        return entity::compatible(address, address_by_key[member]);
                    });
                if (fits) {
                    bucket.push_back(key);
                    placed = true;
                    break;
                }
            }
            if (!placed) buckets.push_back({key});
        }
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            resolved_groups[buckets.size() == 1 ? group : group + "#" + std::to_string(i)] =
                std::move(buckets[i]);
        }
    }
    groups = std::move(resolved_groups);

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
             entity::parse_address(address->second.value).locatable);
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
