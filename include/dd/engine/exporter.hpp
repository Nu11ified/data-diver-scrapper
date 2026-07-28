#pragma once

#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <string>

// Compiles one county's properties into the canonical JSON payload an API
// would serve: for each property the identity, the lifecycle verdict from
// the deterministic state machine, every canonical field's latest value
// with its provenance (which source, which event date), the distress
// signals derived from the event history, and the event log itself.
// Everything in the payload is traceable to an ingested event.
namespace dd::exporter {

std::string county_json(store::Store& store, const schema::Registry& registry,
                        const std::string& county);

} // namespace dd::exporter
