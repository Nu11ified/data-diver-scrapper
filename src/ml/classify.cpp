#include "dd/ml/classify.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"

#include <filesystem>

namespace dd::classify {
namespace {

struct Example {
    std::string label;
    features::Bag bag;
};

std::vector<Example> load_corpus(const std::string& corpus_dir) {
    namespace fs = std::filesystem;
    std::vector<Example> examples;
    if (!fs::is_directory(corpus_dir)) {
        throw Error("classify: corpus directory not found: " + corpus_dir);
    }
    for (const std::string& class_dir : fileio::list_dir(corpus_dir)) {
        if (!fs::is_directory(class_dir)) continue;
        const std::string label = fs::path(class_dir).filename().string();
        for (const std::string& file : fileio::list_dir(class_dir)) {
            if (!fs::is_regular_file(file)) continue;
            const std::string body = fileio::read_file(file);
            const doc::Model model = doc::build_auto("", body);
            // No url during training: corpus file names must not teach the
            // model anything.
            examples.push_back(Example{label, features::extract(model, "")});
        }
    }
    return examples;
}

double leave_one_out(const std::vector<Example>& examples) {
    std::size_t correct = 0;
    for (std::size_t held = 0; held < examples.size(); ++held) {
        model::NaiveBayes bayes;
        for (std::size_t i = 0; i < examples.size(); ++i) {
            if (i != held) bayes.add_example(examples[i].label, examples[i].bag);
        }
        const std::vector<model::Scored> scored = bayes.predict(examples[held].bag);
        if (!scored.empty() && scored.front().label == examples[held].label) ++correct;
    }
    return examples.empty() ? 0.0
                            : static_cast<double>(correct) / static_cast<double>(examples.size());
}

} // namespace

Classifier Classifier::train_from_corpus(const std::string& corpus_dir, TrainReport* report) {
    const std::vector<Example> examples = load_corpus(corpus_dir);
    if (examples.size() < 8) {
        throw Error("classify: corpus too small to train on (" +
                    std::to_string(examples.size()) + " examples)");
    }

    Classifier out;
    out.trained_accuracy_ = leave_one_out(examples);
    for (const Example& example : examples) {
        out.bayes_.add_example(example.label, example.bag);
    }
    out.trained_at_ = timeutil::iso_now();

    if (report != nullptr) {
        report->examples = examples.size();
        report->classes = out.bayes_.class_count();
        report->leave_one_out_accuracy = out.trained_accuracy_;
    }
    return out;
}

void Classifier::save(const std::string& model_path) const {
    json::Writer w;
    w.begin_object();
    w.field("trained_at", trained_at_);
    w.field("leave_one_out_accuracy", trained_accuracy_);
    w.field_raw("model", bayes_.serialize());
    w.end_object();
    fileio::write_file_atomic(model_path, w.str());
}

Classifier Classifier::load(const std::string& model_path) {
    const json::Value root = json::parse(fileio::read_file(model_path));
    const json::Value* inner = root.find("model");
    if (inner == nullptr) throw Error("classify: model file missing 'model'");
    Classifier out;
    out.bayes_ = model::NaiveBayes::deserialize(inner->serialize());
    const json::Value* at = root.find("trained_at");
    if (at != nullptr) out.trained_at_ = at->as_string();
    const json::Value* acc = root.find("leave_one_out_accuracy");
    if (acc != nullptr) out.trained_accuracy_ = acc->as_number();
    return out;
}

Prediction Classifier::classify(const doc::Model& model, const std::string& url) const {
    Prediction out;
    out.distribution = bayes_.predict(features::extract(model, url));
    if (out.distribution.empty()) {
        throw Error("classify: model has no classes; train it first");
    }
    out.label = out.distribution.front().label;
    out.confidence = out.distribution.front().probability;
    return out;
}

} // namespace dd::classify
