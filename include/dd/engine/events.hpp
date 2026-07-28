#pragma once

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

// Deterministic reducer: orders events by (event_date, recorded_at, id) and
// folds them into a lifecycle. Distress events advance the state when their
// rank exceeds the current one; an arm's-length deed transfer resets to
// Normal; permits, violations, assessments and probate are evidence on the
// record but do not move the distress track.
Lifecycle reduce(std::vector<PropertyEvent> events);

} // namespace dd::events
