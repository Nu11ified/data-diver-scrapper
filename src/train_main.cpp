// dd_train: fits the source-type classifier from the labeled corpus and
// writes the model the engine loads at startup. Refuses to ship a model whose
// held-out accuracy is poor, so a degraded corpus fails loudly here rather
// than misclassifying quietly in production.

#include "dd/classify.hpp"

#include <cstdio>
#include <string>

namespace {

constexpr double kMinimumAccuracy = 0.85;

} // namespace

int main(int argc, char** argv) {
    std::string corpus = "data/corpus";
    std::string out = "data/model/source_classifier.json";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--corpus") corpus = argv[i + 1];
        else if (flag == "--out") out = argv[i + 1];
        else {
            std::fprintf(stderr, "usage: dd_train [--corpus DIR] [--out FILE]\n");
            return 2;
        }
    }

    try {
        dd::classify::TrainReport report;
        const dd::classify::Classifier classifier =
            dd::classify::Classifier::train_from_corpus(corpus, &report);
        std::printf("examples: %zu\nclasses: %zu\nleave-one-out accuracy: %.3f\n",
                    report.examples, report.classes, report.leave_one_out_accuracy);
        if (report.leave_one_out_accuracy < kMinimumAccuracy) {
            std::fprintf(stderr,
                         "refusing to save: accuracy %.3f below minimum %.3f\n",
                         report.leave_one_out_accuracy, kMinimumAccuracy);
            return 1;
        }
        classifier.save(out);
        std::printf("model written: %s\n", out.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dd_train: %s\n", e.what());
        return 1;
    }
}
