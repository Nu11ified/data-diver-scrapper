#pragma once

#include "dd/engine/schema.hpp"
#include "dd/ml/columns.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Builds the column-classifier training corpus from live government data
// portals: the Socrata discovery API lists real datasets with their column
// metadata, sample rows supply real values, and columns are weak-labeled by
// exact-synonym matches against the schema (high precision only - anything
// not an exact lexicon hit becomes "none"). The transformer then learns to
// generalise past the lexicon from names and value shapes.
namespace dd::harvest {

struct Options {
    std::vector<std::string> queries;   // catalog search terms
    std::size_t datasets_per_query = 25;
    std::size_t sample_rows = 4;        // rows fetched per dataset for values
    std::size_t max_none_per_dataset = 6;
    long timeout_seconds = 20;
};

Options default_options();

struct Stats {
    std::size_t datasets_seen = 0;
    std::size_t datasets_sampled = 0;
    std::size_t labeled = 0;
    std::size_t none = 0;
    std::size_t masked = 0;
    std::size_t domains = 0;
};

// The weak label for one column. An exact lexicon hit yields the field. A
// near miss - a name that contains lexicon vocabulary without matching it,
// or whose API and display names hit different fields - is masked: it is
// probably a positive we cannot verify, and teaching it as "none" would
// poison the corpus. Only columns with no lexicon contact at all become
// "none" examples.
struct WeakLabel {
    std::string field;    // empty unless an unambiguous exact hit
    bool masked = false;  // drop from the corpus entirely
};
WeakLabel weak_label(const schema::Registry& registry, const std::string& field_name,
                     const std::string& display_name);

// Runs the harvest. `log` receives one line per catalog query and per
// failure; pass nullptr for silence.
std::vector<columns::Example> run(const schema::Registry& registry, const Options& options,
                                  const std::function<void(const std::string&)>& log,
                                  Stats* stats);

// Grows the document-classifier corpus with real datasets: for each class,
// curated catalog queries pull live datasets and their first rows are saved
// under corpus_dir/<class>/. Weak labels come from the query, so the LOO
// confusion table in `train` is the audit: a dataset that lands in the
// wrong class shows up there and the operator deletes the file.
std::map<std::string, std::size_t> grow_corpus(const std::string& corpus_dir,
                                               std::size_t datasets_per_query,
                                               const std::function<void(const std::string&)>& log);

} // namespace dd::harvest
