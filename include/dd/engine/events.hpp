#pragma once

#include "dd/engine/schema.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dd::events {
enum class State {
    Normal,
    TaxDelinquent,
    ForeclosureFiled,
    AuctionScheduled,
    SoldAtAuction,
};

std::string_view state_name(State s);
int state_rank(State s);

enum class Kind {
    TaxDelinquency,
    ForeclosureFiled,
    AuctionScheduled,
    SoldAtAuction,
    DeedTransfer,
    CodeViolation,
    PermitIssued,
    AssessmentRecorded,
    ProbateOpened,
};

std::string_view kind_name(Kind k);

Kind kind_from_source_label(std::string_view label, std::string_view status);

struct PropertyEvent {
    std::string id;           // stable content hash: re-ingestion is idempotent
    std::string property_key;
    Kind kind = Kind::AssessmentRecorded;
    std::string event_date;   // ISO, may be empty when the source had none
    std::string recorded_at;  // when this engine saw it
    std::string source_id;
    std::string run_id;
    std::string as_of;
    double amount = 0.0;      // due/bid/price when the record carried one
    double confidence = 0.0;  // classifier confidence for the source document
    std::map<std::string, std::string> details; // canonical field values

    static std::string compute_id(const PropertyEvent& e);
    std::string serialize() const;
    static PropertyEvent deserialize(const std::string& text);
};

struct Transition {
    State state = State::Normal;
    std::string event_id;
    std::string event_date;
};

struct Lifecycle {
    State state = State::Normal;
    std::vector<Transition> transitions;
};

struct EventContext {
    std::string jurisdiction;
    std::string source_id;
    std::string as_of;
    std::string run_id;
    std::string recorded_at;
};

std::vector<PropertyEvent> build_events(const schema::Registry& registry,
                                        const EventContext& context,
                                        const std::string& classification,
                                        double class_confidence,
                                        const schema::ExtractionResult& extraction);

Lifecycle reduce(std::vector<PropertyEvent> events);
} // namespace dd::events
