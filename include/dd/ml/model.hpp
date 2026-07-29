#pragma once

#include "dd/ml/features.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dd::model {
struct Scored {
    std::string label;
    double probability = 0.0;
};

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

class NaiveBayes {
public:
    void add_example(const std::string& label, const features::Bag& bag);

    std::vector<Scored> predict(const features::Bag& bag) const;

    std::size_t class_count() const noexcept { return classes_.size(); }
    std::size_t example_count() const noexcept { return examples_; }
    std::size_t vocabulary_size() const noexcept { return vocabulary_.size(); }

    std::vector<ClassSummary> summarize(std::size_t top_n) const;

    void set_alpha(double alpha);
    double alpha() const noexcept { return alpha_; }

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
    double alpha_ = 1.0;
};
} // namespace dd::model
