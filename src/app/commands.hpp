#pragma once

#include "dd/engine/pipeline.hpp"
#include "dd/engine/store.hpp"

#include <iosfwd>
#include <string>

// Stateful CLI commands over the store: county views, property tables,
// mapping inspection, the interactive matching review, model status and
// training. Presentation only; all computation lives in the engine.
namespace dd::cli {

void counties(store::Store& store);
void county_properties(store::Store& store, const schema::Registry& registry,
                       const std::string& county, bool include_all);
void show_mapping(store::Store& store, pipeline::Pipeline& pipeline, const std::string& source_id);

// Interactive review: uncertain accepted matches ask for confirmation,
// near-miss candidates for unmapped fields are offered, each with sample
// values from the source's cached bytes. Answers become operator overrides
// and the source reruns. `in` is the answer stream (stdin in the shell,
// scriptable in tests).
void review(store::Store& store, pipeline::Pipeline& pipeline, const std::string& source_id,
            std::istream& in);

void model_status(const classify::Classifier& classifier,
                  const columns::ColumnModel* column_model);

// Grows the document corpus from live catalog datasets and reports what
// landed per class; `train sweep` afterwards is the audit.
int harvest_docs(const std::string& corpus_dir, std::size_t datasets_per_query);

// Trains at one alpha, or sweeps a small grid when sweep=true, printing
// per-class results and the confusion pairs from leave-one-out. Saves the
// model (best one when sweeping) if accuracy clears 0.85 and hot-swaps the
// pipeline's classifier when one is given.
int train(pipeline::Pipeline* pipeline, const std::string& corpus_dir,
          const std::string& model_path, double alpha, bool sweep);

// Harvests the column corpus from live Socrata portals into corpus_path
// (overwriting), then reports what was gathered.
int harvest(const schema::Registry& registry, const std::string& corpus_path,
            std::size_t datasets_per_query);

// Trains the column transformer on the harvested corpus with a by-domain
// holdout, prints per-class results, saves to model_path when the holdout
// accuracy clears 0.75.
int train_columns(pipeline::Pipeline* pipeline, const std::string& corpus_path,
                  const std::string& model_path, int epochs);

// Writes the compiled county payload (exporter::county_json) to out_path,
// or prints it when out_path is empty.
int export_county(store::Store& store, const schema::Registry& registry,
                  const std::string& county, const std::string& out_path);

// The validity benchmark: scores the engine's classification and mapping on
// every golden source against the hand-verified answer key, from cached
// bytes.
int bench(store::Store& store, pipeline::Pipeline& pipeline, const std::string& golden_path);

} // namespace dd::cli
