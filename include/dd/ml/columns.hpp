#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dd::columns {
struct Example {
    std::string name;
    std::vector<std::string> values;
    std::string label;
    std::string domain;
};

std::vector<Example> load_corpus(const std::string& path);
void append_corpus(const std::string& path, const std::vector<Example>& examples);

std::vector<int> tokenize(const std::string& name, const std::vector<std::string>& values,
                          std::size_t max_len);

struct Hyper {
    int seq_len = 64;
    int d_model = 48;
    int heads = 4;
    int layers = 2;
    int d_ffn = 96;
};

struct TrainConfig {
    int epochs = 4;
    int batch = 32;
    double lr = 1e-3;
    std::uint32_t seed = 7;
    int threads = 0;  // 0 = hardware concurrency
};

struct ClassResult {
    std::string label;
    std::size_t total = 0;
    std::size_t correct = 0;
};

struct TrainReport {
    std::size_t train_examples = 0;
    std::size_t holdout_examples = 0;
    std::size_t parameters = 0;
    double holdout_accuracy = 0.0;
    std::vector<double> epoch_loss;
    std::vector<ClassResult> per_class;  // on the holdout
};

struct Prediction {
    std::string label;
    double confidence = 0.0;
    std::map<std::string, double> distribution;
};

class ColumnModel {
public:
    ColumnModel() = default;
    explicit ColumnModel(Hyper hyper) : hyper_(hyper) {}

    TrainReport train(const std::vector<Example>& train, const std::vector<Example>& holdout,
                      const TrainConfig& config);

    Prediction predict(const std::string& name, const std::vector<std::string>& values) const;

    bool trained() const noexcept { return !classes_.empty(); }
    const std::vector<std::string>& classes() const noexcept { return classes_; }
    const Hyper& hyper() const noexcept { return hyper_; }
    std::size_t parameter_count() const noexcept;

    std::string serialize() const;
    static ColumnModel deserialize(const std::string& text);
    void save(const std::string& path) const;
    static ColumnModel load(const std::string& path);

    double loss_for(const Example& example) const;
    std::vector<double> gradient_for(const Example& example) const;
    std::vector<double>& raw_parameters() noexcept { return params_; }

private:
    Hyper hyper_;
    std::vector<std::string> classes_;
    std::vector<double> params_;  // one flat buffer; layout fixed by hyper_
    void init_params(std::uint32_t seed);
};

void split_by_domain(const std::vector<Example>& all, int fold, std::vector<Example>* train,
                     std::vector<Example>* holdout);
} // namespace dd::columns
