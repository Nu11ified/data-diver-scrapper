#pragma once

#include "dd/core/core.hpp"
#include "dd/ml/classify.hpp"
#include "dd/ml/columns.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <string>

namespace dd::pipeline {
std::string expand_url_template(const store::Source& source, store::Store& store);

class Pipeline {
public:
    Pipeline(store::Store& store, classify::Classifier classifier, schema::Registry registry);

    store::RunRecord run_source(const store::Source& source);

    std::vector<store::RunRecord> run_sources(const std::vector<store::Source>& sources,
                                              int threads = 0);

    store::RunRecord run_source_id(const std::string& source_id);

    store::RunRecord run_cached(const store::Source& source);

    const classify::Classifier& classifier() const noexcept { return classifier_; }
    const schema::Registry& registry() const noexcept { return registry_; }

    void set_classifier(classify::Classifier classifier);

    void set_column_model(columns::ColumnModel model);
    const columns::ColumnModel* column_model() const noexcept {
        return column_model_.trained() ? &column_model_ : nullptr;
    }

private:
    store::RunRecord ingest(const store::Source& source, store::RunRecord run,
                            const Stopwatch& total_watch, double cpu_before,
                            const std::string& content_type, const std::string& body);

    store::Store& store_;
    classify::Classifier classifier_;
    schema::Registry registry_;
    columns::ColumnModel column_model_;
};
} // namespace dd::pipeline
