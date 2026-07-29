#pragma once

#include "dd/core/core.hpp"
#include "dd/ml/classify.hpp"
#include "dd/ml/columns.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"
#include "dd/net/fetch.hpp"
#include "dd/parse/document.hpp"

#include <functional>
#include <string>

namespace dd::pipeline {
std::string expand_url_template(const store::Source& source, store::Store& store);

bool is_socrata_resource_url(const std::string& url);

// One HTTP round trip for a single page of a paginated Socrata fetch.
struct SocrataPage {
    bool ok = false;
    std::string error;
    long http_status = 0;
    std::string content_type;
    std::string body;
    std::int64_t bytes = 0;
    double fetch_ms = 0.0;
    fetch::Mode mode = fetch::Mode::Api;
};

// The result of walking $offset across a Socrata resource until a short page,
// a label change (drift), or the record cap.
struct SocrataFetch {
    bool ok = false;
    std::string error;
    long http_status = 0;
    std::int64_t bytes = 0;
    double fetch_ms = 0.0;
    fetch::Mode mode = fetch::Mode::Api;
    bool truncated = false;
    std::string first_page_content_type;
    std::string first_page_body;
    doc::Model model;
};

SocrataFetch fetch_socrata_pages(const std::string& url,
    const std::function<SocrataPage(const std::string& page_url)>& fetch_page);

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
    store::RunRecord ingest_model(const store::Source& source, store::RunRecord run,
                                  const Stopwatch& total_watch, double cpu_before,
                                  doc::Model model);

    store::Store& store_;
    classify::Classifier classifier_;
    schema::Registry registry_;
    columns::ColumnModel column_model_;
};
} // namespace dd::pipeline
