#include "dd/engine/exporter.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"
#include "dd/engine/events.hpp"

#include <algorithm>
#include <set>

namespace dd::exporter {
namespace {

// Latest value of one canonical field across chronologically ordered events,
// with the event that supplied it.
struct Provenanced {
    std::string value;
    std::string source_id;
    std::string event_date;
};

std::map<std::string, Provenanced> latest_fields(const schema::Registry& registry,
                                                 const std::vector<events::PropertyEvent>& evs) {
    std::map<std::string, Provenanced> out;
    for (const events::PropertyEvent& e : evs) {
        for (const schema::FieldDef& field : registry.fields()) {
            const auto it = e.details.find(field.name);
            if (it == e.details.end() || it->second.empty()) continue;
            out[field.name] = Provenanced{it->second, e.source_id, e.event_date};
        }
    }
    return out;
}

} // namespace

std::string county_json(store::Store& store, const schema::Registry& registry,
                        const std::string& county) {
    const std::string wanted = str::slug(county);
    std::vector<std::string> keys;
    for (const std::string& key : store.property_keys()) {
        const std::string slug = key.substr(0, key.find('|'));
        if (slug == wanted || str::contains(slug, wanted)) keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());

    json::Writer w;
    w.begin_object();
    w.field("county", county);
    w.field("generated_at", timeutil::iso_now());
    w.field("properties", static_cast<std::int64_t>(keys.size()));
    w.key("records");
    w.begin_array();
    for (const std::string& key : keys) {
        std::vector<events::PropertyEvent> evs = store.events_for(key);
        std::stable_sort(evs.begin(), evs.end(),
                         [](const events::PropertyEvent& a, const events::PropertyEvent& b) {
                             return a.event_date < b.event_date;
                         });
        const events::Lifecycle life = events::reduce(evs);

        w.begin_object();
        w.field("property_key", key);
        w.field("lifecycle_state", std::string{events::state_name(life.state)});

        std::set<std::string> sources;
        for (const events::PropertyEvent& e : evs) sources.insert(e.source_id);
        w.key("sources");
        w.begin_array();
        for (const std::string& s : sources) w.string_value(s);
        w.end_array();

        w.key("fields");
        w.begin_object();
        for (const auto& [field, p] : latest_fields(registry, evs)) {
            w.key(field);
            w.begin_object();
            w.field("value", p.value);
            w.field("source", p.source_id);
            if (!p.event_date.empty()) w.field("as_of", p.event_date);
            w.end_object();
        }
        w.end_object();

        // Distress signals: facts derived from the event history, the raw
        // material a lead-decisioning layer consumes.
        double delinquent_amount = 0.0;
        double last_sale_amount = 0.0;
        std::int64_t violations = 0;
        std::string auction_date;
        for (const events::PropertyEvent& e : evs) {
            switch (e.kind) {
            case events::Kind::TaxDelinquency: delinquent_amount = e.amount; break;
            case events::Kind::CodeViolation: ++violations; break;
            case events::Kind::AuctionScheduled: auction_date = e.event_date; break;
            case events::Kind::SoldAtAuction:
            case events::Kind::DeedTransfer: last_sale_amount = e.amount; break;
            default: break;
            }
        }
        w.key("signals");
        w.begin_object();
        if (delinquent_amount > 0.0) w.field("delinquent_amount", delinquent_amount);
        if (violations > 0) w.field("code_violations", violations);
        if (!auction_date.empty()) w.field("auction_date", auction_date);
        if (last_sale_amount > 0.0) w.field("last_sale_amount", last_sale_amount);
        w.end_object();

        w.key("events");
        w.begin_array();
        for (const events::PropertyEvent& e : evs) {
            w.begin_object();
            w.field("kind", std::string{events::kind_name(e.kind)});
            if (!e.event_date.empty()) w.field("date", e.event_date);
            w.field("source", e.source_id);
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

} // namespace dd::exporter
