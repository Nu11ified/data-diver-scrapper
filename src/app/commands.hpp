#pragma once

#include "dd/engine/pipeline.hpp"
#include "dd/engine/store.hpp"

#include <iosfwd>
#include <string>

namespace dd::cli {
void counties(store::Store& store);
void county_properties(store::Store& store, const schema::Registry& registry,
                       const std::string& county, bool include_all);
void show_mapping(store::Store& store, pipeline::Pipeline& pipeline, const std::string& source_id);

void review(store::Store& store, pipeline::Pipeline& pipeline, const std::string& source_id,
            std::istream& in);

void model_status(const classify::Classifier& classifier,
                  const columns::ColumnModel* column_model);

int harvest_docs(const std::string& corpus_dir, std::size_t datasets_per_query);

int train(pipeline::Pipeline* pipeline, const std::string& corpus_dir,
          const std::string& model_path, double alpha, bool sweep, double min_accuracy);

int harvest(const schema::Registry& registry, const std::string& corpus_path,
            std::size_t datasets_per_query);

int train_columns(pipeline::Pipeline* pipeline, const std::string& corpus_path,
                  const std::string& model_path, int epochs);

int export_county(store::Store& store, const schema::Registry& registry,
                  const std::string& county, const std::string& out_path);

int catalog(store::Store& store, std::size_t datasets_per_query, bool add);

int freshness(store::Store& store, double stale_hours);

int crawl_site(const schema::Registry& registry, const classify::Classifier& classifier,
               const columns::ColumnModel* column_model, const std::string& seed,
               std::size_t max_pages, std::size_t max_depth);

int bench(store::Store& store, pipeline::Pipeline& pipeline, const std::string& golden_path);
} // namespace dd::cli
