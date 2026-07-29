#pragma once

#include "dd/engine/schema.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dd::events {

// Property lifecycle states. The distress track only advances or resets;
// evidence never silently regresses a property.
enum class State {
    Normal,
    TaxDelinquent,
    ForeclosureFiled,
    AuctionScheduled,
    SoldAtAuction,
};

std::string_view state_name(State s);
int state_rank(State s);

// Event kinds, derived from the source classification of the document the
// record came from.
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

// Maps a classifier label to an event kind. A trustee_auction record whose
// status says the sale happened becomes SoldAtAuction rather than merely
// scheduled.
Kind kind_from_source_label(std::string_view label, std::string_view status);

struct PropertyEvent {
    std::string id;           // stable content hash: re-ingestion is idempotent
    std::string property_key;
    Kind kind = Kind::AssessmentRecorded;
    std::string event_date;   // ISO, may be empty when the source had none
    std::string recorded_at;  // when this engine saw it
    std::string source_id;
    std::string run_id;
    // The edition of the source this event came from; empty for live feeds.
    // Two assessment rolls describe the same property at different times and
    // only this tells them apart, because both carry the same sale date.
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

// Everything about the run that events need; a plain value so event building
// stays pure and callable from any host, including the WASM build where no
// store exists.
struct EventContext {
    std::string jurisdiction;
    std::string source_id;
    std::string as_of;
    std::string run_id;
    std::string recorded_at;
};

// Builds property events from the canonical records of one extraction. Field
// roles from the schema decide identity, dates and amounts.
std::vector<PropertyEvent> build_events(const schema::Registry& registry,
                                        const EventContext& context,
                                        const std::string& classification,
                                        double class_confidence,
                                        const schema::ExtractionResult& extraction);

// Deterministic reducer: orders events by (event_date, recorded_at, id) and
// folds them into a lifecycle. Distress events advance the state when their
// rank exceeds the current one; an arm's-length deed transfer resets to
// Normal; permits, violations, assessments and probate are evidence on the
// record but do not move the distress track.
Lifecycle reduce(std::vector<PropertyEvent> events);

} // namespace dd::events
