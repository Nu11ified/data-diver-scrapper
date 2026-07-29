#pragma once

#include "dd/parse/document.hpp"
#include "dd/ml/model.hpp"

#include <string>
#include <vector>

namespace dd::classify {
struct Prediction {
    std::string label;
    double confidence = 0.0;                  // posterior of the winning class
    std::vector<model::Scored> distribution;  // full posterior, sorted
};

struct LooPrediction {
    std::string actual;
    std::string predicted;
    std::string source;  // corpus file, so disagreements can be inspected
};

struct TrainReport {
    std::size_t examples = 0;
    std::size_t classes = 0;
    double leave_one_out_accuracy = 0.0;
    std::vector<LooPrediction> predictions; // one per corpus document
};

class Classifier {
public:
    static Classifier train_from_corpus(const std::string& corpus_dir, TrainReport* report,
                                        double alpha = 1.0);

    static Classifier load(const std::string& model_path);
    static Classifier from_json(const std::string& text);
    void save(const std::string& model_path) const;

    Prediction classify(const doc::Model& model, const std::string& url) const;

    const std::string& trained_at() const noexcept { return trained_at_; }
    double trained_accuracy() const noexcept { return trained_accuracy_; }
    std::size_t example_count() const noexcept { return bayes_.example_count(); }
    const model::NaiveBayes& bayes() const noexcept { return bayes_; }

private:
    model::NaiveBayes bayes_;
    std::string trained_at_;
    double trained_accuracy_ = 0.0;
};
} // namespace dd::classify
