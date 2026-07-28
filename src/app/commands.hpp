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
                       const std::string& county);
void show_mapping(store::Store& store, pipeline::Pipeline& pipeline, const std::string& source_id);

// Interactive review: uncertain accepted matches ask for confirmation,
// near-miss candidates for unmapped fields are offered, each with sample
// values from the source's cached bytes. Answers become operator overrides
// and the source reruns. `in` is the answer stream (stdin in the shell,
// scriptable in tests).
void review(store::Store& store, pipeline::Pipeline& pipeline, const std::string& source_id,
            std::istream& in);

void model_status(const classify::Classifier& classifier);

// Trains at one alpha, or sweeps a small grid when sweep=true, printing
// per-class results and the confusion pairs from leave-one-out. Saves the
// model (best one when sweeping) if accuracy clears 0.85 and hot-swaps the
// pipeline's classifier when one is given.
int train(pipeline::Pipeline* pipeline, const std::string& corpus_dir,
          const std::string& model_path, double alpha, bool sweep);

// The validity benchmark: scores the engine's classification and mapping on
// every golden source against the hand-verified answer key, from cached
// bytes. with_llm additionally runs the same documents and the same answer
// key through the env-configured LLM baseline and prints both side by side.
int bench(store::Store& store, pipeline::Pipeline& pipeline, const std::string& golden_path,
          bool with_llm);

} // namespace dd::cli
