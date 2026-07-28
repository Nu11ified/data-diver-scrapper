#pragma once

#include "dd/classify.hpp"
#include "dd/store.hpp"

#include <string>

namespace dd::pipeline {

// Runs the full ingestion for sources: fetch, detect, extract, classify,
// map (learning or healing the mapping as needed), resolve to properties,
// and append events. Produces one RunRecord per attempt with only measured
// numbers on it; failures surface on the record with the stage that failed.
class Pipeline {
public:
    Pipeline(store::Store& store, classify::Classifier classifier);

    store::RunRecord run_source(const store::Source& source);

    // Convenience: run by id. Throws dd::Error for an unknown source.
    store::RunRecord run_source_id(const std::string& source_id);

    const classify::Classifier& classifier() const noexcept { return classifier_; }

private:
    store::Store& store_;
    classify::Classifier classifier_;
};

} // namespace dd::pipeline
