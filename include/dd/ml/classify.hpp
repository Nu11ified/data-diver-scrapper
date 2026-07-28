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

// One leave-one-out probe: what the corpus says the document is, and what a
// model trained on every other document called it.
struct LooPrediction {
    std::string actual;
    std::string predicted;
};

struct TrainReport {
    std::size_t examples = 0;
    std::size_t classes = 0;
    double leave_one_out_accuracy = 0.0;
    std::vector<LooPrediction> predictions; // one per corpus document
};

// Source-type classifier: what kind of public-record document is this?
// Labels come from the training corpus directory names.
class Classifier {
public:
    // corpus_dir contains one subdirectory per label, each holding example
    // documents in any supported format. Throws dd::Error when the corpus is
    // missing or too small to train on.
    // alpha is the naive Bayes smoothing strength used for both the
    // leave-one-out folds and the final fit, so the reported accuracy is the
    // accuracy of the model that ships.
    static Classifier train_from_corpus(const std::string& corpus_dir, TrainReport* report,
                                        double alpha = 1.0);

    static Classifier load(const std::string& model_path);
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
