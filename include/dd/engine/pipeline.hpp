#pragma once

#include "dd/core/core.hpp"
#include "dd/ml/classify.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <string>

namespace dd::pipeline {

// Runs the full ingestion for sources: fetch, detect, extract, classify,
// map (learning or healing the mapping as needed, then applying operator
// overrides), resolve to properties, and append events. Produces one
// RunRecord per attempt with only measured numbers on it; failures surface
// on the record with the stage that failed.
class Pipeline {
public:
    // The registry is the canonical schema this pipeline fills: the set of
    // labels, their validators and their event roles, loaded from JSON.
    Pipeline(store::Store& store, classify::Classifier classifier, schema::Registry registry);

    store::RunRecord run_source(const store::Source& source);

    // Convenience: run by id. Throws dd::Error for an unknown source.
    store::RunRecord run_source_id(const std::string& source_id);

    // Replays the cached bytes of the source's last successful fetch through
    // the full downstream pipeline (parse, classify, map, resolve). Throws
    // dd::Error when the source has no fetch cache.
    store::RunRecord run_cached(const store::Source& source);

    const classify::Classifier& classifier() const noexcept { return classifier_; }
    const schema::Registry& registry() const noexcept { return registry_; }

    // Hot-swaps the live classifier. Callers serialize this with runs.
    void set_classifier(classify::Classifier classifier);

private:
    store::RunRecord ingest(const store::Source& source, store::RunRecord run,
                            const Stopwatch& total_watch, double cpu_before,
                            const std::string& content_type, const std::string& body);

    store::Store& store_;
    classify::Classifier classifier_;
    schema::Registry registry_;
};

} // namespace dd::pipeline
