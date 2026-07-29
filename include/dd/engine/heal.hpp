#pragma once

#include "dd/parse/document.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <string>
#include <vector>

namespace dd::heal {
struct Assessment {
    bool drift = false;
    std::string reason;
    double baseline_rate = 0.0;
    double current_rate = 0.0;
    bool fingerprint_changed = false;
};

Assessment assess(const store::SourceState& state, const doc::Model& model,
                  const schema::ExtractionResult& with_stored_mapping);

struct Proposal {
    schema::Mapping candidate;
    schema::ExtractionResult result;
    double confidence = 0.0;  // combined mapping confidence and recovery rate
    bool acceptable = false;  // clears the auto-accept bar; else review queue
    std::vector<std::string> changes; // human-readable before -> after
};

Proposal propose(const schema::Registry& registry, const doc::Model& model,
                 const schema::Mapping& previous, double baseline_rate,
                 const columns::ColumnModel* neural = nullptr);

double auto_accept_threshold();
} // namespace dd::heal
