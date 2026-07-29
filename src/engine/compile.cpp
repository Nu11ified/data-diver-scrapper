#include "dd/engine/compile.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"
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

            std::vector<Observation>& seen = property->history[field.name];
            const bool already = std::any_of(
                seen.begin(), seen.end(), [&](const Observation& o) {
                    return o.as_of == e.as_of && o.source_id == e.source_id;
                });
            if (!already) seen.push_back(Observation{e.as_of, it->second, e.source_id});

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
    std::map<std::string, std::vector<events::PropertyEvent>> events_by_key;
    for (const std::string& key : store.property_keys()) {
        events_by_key[key] = store.events_for(key);
    }
    return county_from_events(registry, events_by_key, source_trust(store), county);
}

std::vector<Property> county_from_events(
    const schema::Registry& registry,
    const std::map<std::string, std::vector<events::PropertyEvent>>& all_events,
    const std::map<std::string, std::map<std::string, double>>& trust,
    const std::string& county) {
    const std::string wanted = str::slug(county);

    std::map<std::string, std::vector<std::string>> groups;
    std::map<std::string, std::vector<events::PropertyEvent>> events_by_key;
    std::map<std::string, entity::Address> address_by_key;
    for (const auto& [key, all_evs] : all_events) {
        const std::string slug = key.substr(0, key.find('|'));
        if (slug != wanted && !str::contains(slug, wanted)) continue;
        std::vector<events::PropertyEvent> evs = all_evs;
        const entity::Address address = entity::parse_address(first_address(evs));
        const std::string join = entity::address_join_key(address);
        groups[join.empty() ? key : slug + "|a:" + join].push_back(key);
        address_by_key[key] = address;
        events_by_key[key] = std::move(evs);
    }

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

std::string render_county_json(const std::string& county,
                               const std::vector<Property>& properties) {
    std::size_t without_address = 0;
    for (const Property& p : properties) {
        if (!p.locates_a_building) ++without_address;
    }

    json::Writer w;
    w.begin_object();
    w.field("county", county);
    w.field("generated_at", timeutil::iso_now());
    w.field("properties", static_cast<std::int64_t>(properties.size() - without_address));
    w.field("hidden_unlocatable", static_cast<std::int64_t>(without_address));
    w.key("records");
    w.begin_array();
    for (const Property& p : properties) {
        if (!p.locates_a_building) continue;
        w.begin_object();
        w.key("keys");
        w.begin_array();
        for (const std::string& key : p.keys) w.string_value(key);
        w.end_array();
        w.field("lifecycle_state", std::string{events::state_name(p.state)});

        w.key("fields");
        w.begin_object();
        for (const auto& [field, resolved] : p.fields) {
            w.key(field);
            w.begin_object();
            w.field("value", resolved.value);
            w.field("source", resolved.source_id);
            w.field("confidence", resolved.confidence);
            if (!resolved.as_of.empty()) w.field("edition", resolved.as_of);
            if (!resolved.event_date.empty()) w.field("as_of", resolved.event_date);
            w.end_object();
        }
        w.end_object();

        if (!p.conflicts.empty()) {
            w.key("conflicts");
            w.begin_array();
            for (const Conflict& c : p.conflicts) {
                w.begin_object();
                w.field("field", c.field);
                w.field("kept", c.kept_value);
                w.field("kept_source", c.kept_source);
                w.field("kept_confidence", c.kept_confidence);
                w.field("dropped", c.dropped_value);
                w.field("dropped_source", c.dropped_source);
                w.field("dropped_confidence", c.dropped_confidence);
                w.end_object();
            }
            w.end_array();
        }

        w.key("signals");
        w.begin_object();
        if (p.due > 0.0) w.field("delinquent_amount", p.due);
        if (p.violations > 0) {
            w.field("code_violations", static_cast<std::int64_t>(p.violations));
        }
        if (!p.auction_date.empty()) w.field("auction_date", p.auction_date);
        if (p.assessed > 0.0) w.field("assessed_value", p.assessed);
        if (p.due > 0.0 && p.assessed > 0.0) w.field("debt_to_value", p.due / p.assessed);
        if (p.assessed_previous > 0.0) {
            w.field("assessed_value_previous", p.assessed_previous);
            if (p.assessed > 0.0) {
                w.field("assessed_value_change",
                        (p.assessed - p.assessed_previous) / p.assessed_previous);
            }
        }
        w.end_object();

        w.key("events");
        w.begin_array();
        for (const events::PropertyEvent& e : p.events) {
            w.begin_object();
            w.field("kind", std::string{events::kind_name(e.kind)});
            if (!e.event_date.empty()) w.field("date", e.event_date);
            w.field("source", e.source_id);
            if (!e.as_of.empty()) w.field("edition", e.as_of);
            if (e.amount > 0.0) w.field("amount", e.amount);
            w.field("confidence", e.confidence);
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.take();
}
} // namespace dd::compile
