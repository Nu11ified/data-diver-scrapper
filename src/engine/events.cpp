#include "dd/engine/events.hpp"

#include "dd/engine/entity.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"

#include <algorithm>

namespace dd::events {
std::string_view state_name(State s) {
    switch (s) {
    case State::Normal: return "NORMAL";
    case State::TaxDelinquent: return "TAX_DELINQUENT";
    case State::ForeclosureFiled: return "FORECLOSURE_FILED";
    case State::AuctionScheduled: return "AUCTION_SCHEDULED";
    case State::SoldAtAuction: return "SOLD_AT_AUCTION";
    }
    return "NORMAL";
}

int state_rank(State s) { return static_cast<int>(s); }

std::string_view kind_name(Kind k) {
    switch (k) {
    case Kind::TaxDelinquency: return "tax_delinquency";
    case Kind::ForeclosureFiled: return "foreclosure_filed";
    case Kind::AuctionScheduled: return "auction_scheduled";
    case Kind::SoldAtAuction: return "sold_at_auction";
    case Kind::DeedTransfer: return "deed_transfer";
    case Kind::CodeViolation: return "code_violation";
    case Kind::PermitIssued: return "permit_issued";
    case Kind::AssessmentRecorded: return "assessment_recorded";
    case Kind::ProbateOpened: return "probate_opened";
    }
    return "assessment_recorded";
}

Kind kind_from_source_label(std::string_view label, std::string_view status) {
    const std::string lowered_status = str::to_lower(status);
    if (label == "tax_delinquency") return Kind::TaxDelinquency;
    if (label == "foreclosure_filing") return Kind::ForeclosureFiled;
    if (label == "trustee_auction") {
        if (str::contains(lowered_status, "sold")) return Kind::SoldAtAuction;
        return Kind::AuctionScheduled;
    }
    if (label == "deed_transfer") return Kind::DeedTransfer;
    if (label == "code_violation") return Kind::CodeViolation;
    if (label == "building_permit") return Kind::PermitIssued;
    if (label == "assessor_roll") return Kind::AssessmentRecorded;
    if (label == "probate_case") return Kind::ProbateOpened;
    return Kind::AssessmentRecorded;
}

std::string PropertyEvent::compute_id(const PropertyEvent& e) {
    std::string basis = e.property_key;
    basis.push_back('|');
    basis += kind_name(e.kind);
    basis.push_back('|');
    basis += e.event_date;
    basis.push_back('|');
    basis += e.source_id;
    const auto it = e.details.find("case_number");
    if (it != e.details.end()) {
        basis.push_back('|');
        basis += it->second;
    }
    return str::hex64(str::hash64(basis));
}

std::string PropertyEvent::serialize() const {
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.field("property_key", property_key);
    w.field("kind", kind_name(kind));
    w.field("event_date", event_date);
    w.field("recorded_at", recorded_at);
    w.field("source_id", source_id);
    w.field("source_label", source_label);
    w.field("as_of", as_of);
    w.field("run_id", run_id);
    w.field("amount", amount);
    w.field("confidence", confidence);
    w.key("details");
    w.begin_object();
    for (const auto& [key, value] : details) w.field(key, value);
    w.end_object();
    w.end_object();
    return w.take();
}

PropertyEvent PropertyEvent::deserialize(const std::string& text) {
    const json::Value root = json::parse(text);
    PropertyEvent e;
    auto get = [&](const char* key) -> std::string {
        const json::Value* v = root.find(key);
        return v == nullptr ? std::string{} : v->as_string();
    };
    e.id = get("id");
    e.property_key = get("property_key");
    e.event_date = get("event_date");
    e.recorded_at = get("recorded_at");
    e.source_id = get("source_id");
    e.source_label = get("source_label");
    e.as_of = get("as_of");
    e.run_id = get("run_id");
    const json::Value* amount = root.find("amount");
    if (amount != nullptr) e.amount = amount->as_number();
    const json::Value* confidence = root.find("confidence");
    if (confidence != nullptr) e.confidence = confidence->as_number();

    const std::string kind = get("kind");
    bool matched = false;
    for (const Kind k : {Kind::TaxDelinquency, Kind::ForeclosureFiled, Kind::AuctionScheduled,
                         Kind::SoldAtAuction, Kind::DeedTransfer, Kind::CodeViolation,
                         Kind::PermitIssued, Kind::AssessmentRecorded, Kind::ProbateOpened}) {
        if (kind_name(k) == kind) {
            e.kind = k;
            matched = true;
            break;
        }
    }
    if (!matched) throw Error("events: unknown kind '" + kind + "'");

    const json::Value* details = root.find("details");
    if (details != nullptr && details->is_object()) {
        for (const auto& [key, value] : details->members()) {
            e.details[key] = value.as_string();
        }
    }
    if (e.id.empty() || e.property_key.empty()) {
        throw Error("events: event missing id or property_key");
    }
    return e;
}

Lifecycle reduce(std::vector<PropertyEvent> events) {
    std::sort(events.begin(), events.end(), [](const PropertyEvent& a, const PropertyEvent& b) {
        if (a.event_date != b.event_date) return a.event_date < b.event_date;
        if (a.recorded_at != b.recorded_at) return a.recorded_at < b.recorded_at;
        return a.id < b.id;
    });

    Lifecycle out;
    for (const PropertyEvent& e : events) {
        State proposed = out.state;
        switch (e.kind) {
        case Kind::TaxDelinquency: proposed = State::TaxDelinquent; break;
        case Kind::ForeclosureFiled: proposed = State::ForeclosureFiled; break;
        case Kind::AuctionScheduled: proposed = State::AuctionScheduled; break;
        case Kind::SoldAtAuction: proposed = State::SoldAtAuction; break;
        case Kind::DeedTransfer: proposed = State::Normal; break;
        case Kind::CodeViolation:
        case Kind::PermitIssued:
        case Kind::AssessmentRecorded:
        case Kind::ProbateOpened: continue; // evidence only
        }

        const bool reset = e.kind == Kind::DeedTransfer;
        const bool advance = state_rank(proposed) > state_rank(out.state);
        if (!reset && !advance) continue;
        if (reset && out.state == State::Normal) continue;

        out.state = reset ? State::Normal : proposed;
        out.transitions.push_back(Transition{out.state, e.id, e.event_date});
    }
    return out;
}

namespace {
std::string pick_role(const schema::Registry& registry,
                      const std::map<std::string, std::string>& values,
                      std::string_view role) {
    for (const schema::FieldDef* field : registry.with_role(role)) {
        const auto it = values.find(field->name);
        if (it != values.end() && !it->second.empty()) return it->second;
    }
    return {};
}
} // namespace

std::vector<PropertyEvent> build_events(const schema::Registry& registry,
                                        const EventContext& context,
                                        const std::string& classification,
                                        double class_confidence,
                                        const schema::ExtractionResult& extraction) {
    std::vector<PropertyEvent> out;
    for (const schema::CanonicalRecord& record : extraction.records) {
        const std::string parcel = pick_role(registry, record.values, "parcel");
        const std::string address = pick_role(registry, record.values, "address");
        const std::string key = entity::property_key(context.jurisdiction, parcel, address);
        if (key.empty()) continue; // unresolvable: no identity evidence

        PropertyEvent e;
        e.property_key = key;
        e.kind = kind_from_source_label(classification,
                                        pick_role(registry, record.values, "status"));
        e.event_date = pick_role(registry, record.values, "event_date");
        if (e.event_date.empty()) {
            e.event_date = pick_role(registry, record.values, "fallback_date");
        }
        e.recorded_at = context.recorded_at;
        e.source_id = context.source_id;
        e.source_label = classification;
        e.as_of = context.as_of;
        e.run_id = context.run_id;
        e.confidence = class_confidence;
        const std::string amount = pick_role(registry, record.values, "amount");
        if (!amount.empty()) e.amount = std::atof(amount.c_str());
        e.details = record.values;
        e.id = PropertyEvent::compute_id(e);
        out.push_back(std::move(e));
    }
    return out;
}
} // namespace dd::events
