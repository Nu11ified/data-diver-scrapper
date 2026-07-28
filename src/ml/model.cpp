#include "dd/ml/model.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <cmath>

namespace dd::model {

NaiveBayes::Class& NaiveBayes::class_for(const std::string& label) {
    for (Class& c : classes_) {
        if (c.name == label) return c;
    }
    classes_.push_back(Class{label, 0, 0, {}});
    return classes_.back();
}

void NaiveBayes::add_example(const std::string& label, const features::Bag& bag) {
    Class& c = class_for(label);
    ++c.documents;
    ++examples_;
    for (const auto& [token, count] : bag) {
        c.counts[token] += count;
        c.tokens += count;
        vocabulary_[token] = true;
    }
}

std::vector<Scored> NaiveBayes::predict(const features::Bag& bag) const {
    std::vector<Scored> out;
    if (classes_.empty() || examples_ == 0) return out;

    const double vocab = static_cast<double>(std::max<std::size_t>(vocabulary_.size(), 1));
    std::vector<double> log_scores;
    log_scores.reserve(classes_.size());

    for (const Class& c : classes_) {
        double score = std::log(static_cast<double>(c.documents) /
                                static_cast<double>(examples_));
        const double denom = static_cast<double>(c.tokens) + vocab;
        for (const auto& [token, count] : bag) {
            if (vocabulary_.find(token) == vocabulary_.end()) continue;
            const auto it = c.counts.find(token);
            const double hits = it == c.counts.end() ? 0.0 : static_cast<double>(it->second);
            score += static_cast<double>(count) * std::log((hits + 1.0) / denom);
        }
        log_scores.push_back(score);
    }

    // Softmax in log space for a proper posterior.
    const double max_log = *std::max_element(log_scores.begin(), log_scores.end());
    double total = 0.0;
    for (double& s : log_scores) {
        s = std::exp(s - max_log);
        total += s;
    }
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        out.push_back(Scored{classes_[i].name, log_scores[i] / total});
    }
    std::sort(out.begin(), out.end(),
              [](const Scored& a, const Scored& b) { return a.probability > b.probability; });
    return out;
}

std::vector<ClassSummary> NaiveBayes::summarize(std::size_t top_n) const {
    // Corpus-wide token totals for the lift denominator.
    std::map<std::string, std::int64_t> corpus_counts;
    std::int64_t corpus_tokens = 0;
    for (const Class& c : classes_) {
        for (const auto& [token, count] : c.counts) corpus_counts[token] += count;
        corpus_tokens += c.tokens;
    }

    std::vector<ClassSummary> out;
    for (const Class& c : classes_) {
        ClassSummary summary;
        summary.name = c.name;
        summary.documents = c.documents;
        summary.tokens = c.tokens;
        if (c.tokens == 0 || corpus_tokens == 0) {
            out.push_back(std::move(summary));
            continue;
        }
        std::vector<TokenWeight> weights;
        for (const auto& [token, count] : c.counts) {
            if (count < 2) continue; // one-offs are noise, not vocabulary
            const double in_class =
                static_cast<double>(count) / static_cast<double>(c.tokens);
            const double in_corpus = static_cast<double>(corpus_counts[token]) /
                                     static_cast<double>(corpus_tokens);
            weights.push_back(TokenWeight{token, count, in_class / in_corpus});
        }
        // Class-exclusive tokens all share the maximum lift, so rank by
        // lift weighted with log frequency: the vocabulary the class uses
        // often comes first.
        auto score = [](const TokenWeight& t) {
            return t.lift * std::log1p(static_cast<double>(t.count));
        };
        std::sort(weights.begin(), weights.end(),
                  [&](const TokenWeight& a, const TokenWeight& b) {
                      const double sa = score(a);
                      const double sb = score(b);
                      if (sa != sb) return sa > sb;
                      return a.token < b.token;
                  });
        if (weights.size() > top_n) weights.resize(top_n);
        summary.top_tokens = std::move(weights);
        out.push_back(std::move(summary));
    }
    return out;
}

std::string NaiveBayes::serialize() const {
    json::Writer w;
    w.begin_object();
    w.field("kind", "naive_bayes_multinomial");
    w.field("version", 1);
    w.field("examples", static_cast<std::int64_t>(examples_));
    w.key("classes");
    w.begin_array();
    for (const Class& c : classes_) {
        w.begin_object();
        w.field("name", c.name);
        w.field("documents", c.documents);
        w.field("tokens", c.tokens);
        w.key("counts");
        w.begin_object();
        for (const auto& [token, count] : c.counts) w.field(token, count);
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.take();
}

NaiveBayes NaiveBayes::deserialize(const std::string& text) {
    const json::Value root = json::parse(text);
    const json::Value* kind = root.find("kind");
    if (kind == nullptr || kind->as_string() != "naive_bayes_multinomial") {
        throw Error("model: unrecognized serialization kind");
    }
    NaiveBayes nb;
    const json::Value* examples = root.find("examples");
    nb.examples_ = examples == nullptr ? 0 : static_cast<std::size_t>(examples->as_number());
    const json::Value* classes = root.find("classes");
    if (classes == nullptr || !classes->is_array()) throw Error("model: missing classes");
    for (const json::Value& entry : classes->items()) {
        Class c;
        const json::Value* name = entry.find("name");
        if (name == nullptr) throw Error("model: class without name");
        c.name = name->as_string();
        c.documents = static_cast<std::int64_t>(entry.find("documents")->as_number());
        c.tokens = static_cast<std::int64_t>(entry.find("tokens")->as_number());
        const json::Value* counts = entry.find("counts");
        if (counts != nullptr && counts->is_object()) {
            for (const auto& [token, count] : counts->members()) {
                c.counts[token] = static_cast<std::int64_t>(count.as_number());
                nb.vocabulary_[token] = true;
            }
        }
        nb.classes_.push_back(std::move(c));
    }
    if (nb.classes_.empty()) throw Error("model: no classes in serialized model");
    return nb;
}

} // namespace dd::model
