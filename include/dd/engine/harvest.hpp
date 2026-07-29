#pragma once

#include "dd/engine/schema.hpp"
#include "dd/ml/columns.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

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

struct WeakLabel {
    std::string field;    // empty unless an unambiguous exact hit
    bool masked = false;  // drop from the corpus entirely
};
WeakLabel weak_label(const schema::Registry& registry, const std::string& field_name,
                     const std::string& display_name);

std::vector<columns::Example> run(const schema::Registry& registry, const Options& options,
                                  const std::function<void(const std::string&)>& log,
                                  Stats* stats);

std::map<std::string, std::size_t> grow_corpus(const std::string& corpus_dir,
                                               std::size_t datasets_per_query,
                                               const std::function<void(const std::string&)>& log);

struct Discovered {
    std::string id, name, url, jurisdiction, query;
};
std::vector<Discovered> discover(std::size_t datasets_per_query,
                                 const std::function<void(const std::string&)>& log);
} // namespace dd::harvest
