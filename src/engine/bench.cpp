#include "dd/engine/bench.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"

#include <algorithm>

namespace dd::bench {
namespace {

std::vector<std::string> string_or_list(const json::Value& value) {
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const json::Value& item : value.items()) out.push_back(item.as_string());
    } else {
        out.push_back(value.as_string());
    }
    return out;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

std::vector<Golden> load_golden(const std::string& path) {
    const json::Value root = json::parse(fileio::read_file(path));
    const json::Value* sources = root.find("sources");
    if (sources == nullptr || !sources->is_array()) {
        throw Error("golden: expected a top-level \"sources\" array in " + path);
    }
    std::vector<Golden> out;
    for (const json::Value& entry : sources->items()) {
        Golden g;
        const json::Value* id = entry.find("id");
        const json::Value* cls = entry.find("classification");
        if (id == nullptr || cls == nullptr) {
            throw Error("golden: every source needs \"id\" and \"classification\"");
        }
        g.source_id = id->as_string();
        g.classifications = string_or_list(*cls);
        if (const json::Value* fields = entry.find("fields"); fields != nullptr) {
            for (const auto& [field, labels] : fields->members()) {
                g.fields[field] = string_or_list(labels);
            }
        }
        out.push_back(std::move(g));
    }
    return out;
}

double MappingScore::precision() const noexcept {
    return tp + spurious == 0 ? 1.0 : static_cast<double>(tp) / static_cast<double>(tp + spurious);
}

double MappingScore::recall() const noexcept {
    return tp + missing == 0 ? 1.0 : static_cast<double>(tp) / static_cast<double>(tp + missing);
}

double MappingScore::f1() const noexcept {
    const double p = precision();
    const double r = recall();
    return p + r == 0.0 ? 0.0 : 2.0 * p * r / (p + r);
}

bool classification_ok(const Golden& golden, const std::string& predicted) {
    return contains(golden.classifications, predicted);
}

MappingScore score_mapping(const Golden& golden, const schema::Registry& registry,
                           const std::map<std::string, std::string>& mapped) {
    MappingScore score;
    for (const schema::FieldDef& field : registry.fields()) {
        const auto golden_it = golden.fields.find(field.name);
        static const std::vector<std::string> kUnmappedOnly{""};
        const std::vector<std::string>& acceptable =
            golden_it == golden.fields.end() ? kUnmappedOnly : golden_it->second;

        const auto mapped_it = mapped.find(field.name);
        const std::string label = mapped_it == mapped.end() ? "" : mapped_it->second;

        if (label.empty()) {
            const bool required =
                !contains(acceptable, "") &&
                std::any_of(acceptable.begin(), acceptable.end(),
                            [](const std::string& l) { return !l.empty(); });
            if (required) ++score.missing;
        } else if (contains(acceptable, label)) {
            ++score.tp;
        } else {
            ++score.spurious;
        }
    }
    return score;
}

} // namespace dd::bench
