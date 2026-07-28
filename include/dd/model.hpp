#pragma once

#include "dd/features.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dd::model {

struct Scored {
    std::string label;
    double probability = 0.0;
};

// Introspection: what one class learned. Top tokens are ranked by lift, the
// ratio of the token's in-class frequency to its corpus frequency, so the
// discriminative vocabulary surfaces instead of the words every county page
// shares.
struct TokenWeight {
    std::string token;
    std::int64_t count = 0;
    double lift = 0.0;
};

struct ClassSummary {
    std::string name;
    std::int64_t documents = 0;
    std::int64_t tokens = 0;
    std::vector<TokenWeight> top_tokens;
};

// Multinomial naive Bayes with Laplace smoothing. Small, fast, and honest
// about uncertainty: predict() returns a posterior distribution, and the
// probability of the winning class is the confidence the UI shows.
class NaiveBayes {
public:
    void add_example(const std::string& label, const features::Bag& bag);

    // Sorted by probability, highest first. Tokens never seen in training are
    // ignored rather than letting smoothing skew towards small classes.
    std::vector<Scored> predict(const features::Bag& bag) const;

    std::size_t class_count() const noexcept { return classes_.size(); }
    std::size_t example_count() const noexcept { return examples_; }
    std::size_t vocabulary_size() const noexcept { return vocabulary_.size(); }

    std::vector<ClassSummary> summarize(std::size_t top_n) const;

    std::string serialize() const;
    static NaiveBayes deserialize(const std::string& text);

private:
    struct Class {
        std::string name;
        std::int64_t documents = 0;
        std::int64_t tokens = 0;
        std::map<std::string, std::int64_t> counts;
    };

    Class& class_for(const std::string& label);

    std::vector<Class> classes_;
    std::map<std::string, bool> vocabulary_;
    std::size_t examples_ = 0;
};

} // namespace dd::model
