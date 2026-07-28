#pragma once

#include "dd/parse/document.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <string>
#include <vector>

namespace dd::heal {

// Verdict on whether a source has drifted away from its accepted mapping.
struct Assessment {
    bool drift = false;
    std::string reason;
    double baseline_rate = 0.0;
    double current_rate = 0.0;
    bool fingerprint_changed = false;
};

// Drift means the stored mapping stopped doing its job: the extraction rate
// collapsed relative to the learned baseline, or the document changed shape
// and the rate dropped with it. A quiet content update that still extracts
// cleanly is not drift.
Assessment assess(const store::SourceState& state, const doc::Model& model,
                  const schema::ExtractionResult& with_stored_mapping);

struct Proposal {
    schema::Mapping candidate;
    schema::ExtractionResult result;
    double confidence = 0.0;  // combined mapping confidence and recovery rate
    bool acceptable = false;  // clears the auto-accept bar; else review queue
    std::vector<std::string> changes; // human-readable before -> after
};

// Searches the drifted document for a replacement mapping: extraction has
// already collected every labelled cell (data attributes, classes, label
// text, headers), so the healer rescores those labels against the field
// lexicon and validates the values they produce. Acceptance requires both a
// confident mapping and recovery of most of the baseline extraction rate.
Proposal propose(const doc::Model& model, const schema::Mapping& previous,
                 double baseline_rate);

// Auto-accept bar for repairs. Exposed so tests and the UI can state it.
double auto_accept_threshold();

} // namespace dd::heal
