#include "dd/engine/exporter.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"
#include "dd/engine/compile.hpp"
#include "dd/engine/events.hpp"

namespace dd::exporter {

std::string county_json(store::Store& store, const schema::Registry& registry,
                        const std::string& county) {
    const std::vector<compile::Property> properties =
        compile::county(store, registry, county);

    std::size_t without_address = 0;
    for (const compile::Property& p : properties) {
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
    for (const compile::Property& p : properties) {
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
            for (const compile::Conflict& c : p.conflicts) {
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
