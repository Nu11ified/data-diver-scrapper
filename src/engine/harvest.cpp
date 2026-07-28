#include "dd/engine/harvest.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"
#include "dd/net/fetch.hpp"

#include <set>

namespace dd::harvest {
namespace {

constexpr const char* kCatalog = "https://api.us.socrata.com/api/catalog/v1";

std::string url_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", c);
            out += buffer;
        }
    }
    return out;
}

// A scalar cell rendered as the string a scraper would see.
std::string cell_text(const json::Value& v) {
    if (!v.is_scalar()) return {};
    return v.as_string();
}

} // namespace

Options default_options() {
    Options o;
    o.queries = {
        "property tax",        "tax sale",          "delinquent",
        "code violations",     "building permits",  "property assessment",
        "foreclosure",         "sheriff sale",      "deed",
        "parcel",              "housing inspection", "business licenses",
        "restaurant inspections", "crime reports",
    };
    return o;
}

std::string weak_label(const schema::Registry& registry, const std::string& field_name,
                       const std::string& display_name) {
    const std::string field_slug = str::slug(field_name);
    const std::string display_slug = str::slug(display_name);
    for (const schema::FieldDef& field : registry.fields()) {
        if (str::slug(field.name) == field_slug || str::slug(field.name) == display_slug) {
            return field.name;
        }
        for (const std::string& synonym : field.synonyms) {
            const std::string synonym_slug = str::slug(synonym);
            if (synonym_slug == field_slug || synonym_slug == display_slug) return field.name;
        }
    }
    return {};
}

std::vector<columns::Example> run(const schema::Registry& registry, const Options& options,
                                  const std::function<void(const std::string&)>& log,
                                  Stats* stats) {
    fetch::Options net;
    net.timeout_seconds = options.timeout_seconds;
    std::vector<columns::Example> out;
    std::set<std::string> seen_datasets;
    std::set<std::string> domains;
    Stats local;

    for (const std::string& query : options.queries) {
        const std::string url = std::string{kCatalog} + "?only=datasets&limit=" +
                                std::to_string(options.datasets_per_query) +
                                "&q=" + url_encode(query);
        const fetch::Result catalog = fetch::get(url, net);
        if (!catalog.ok) {
            if (log) log("catalog query '" + query + "' failed: " + catalog.error);
            continue;
        }
        json::Value root;
        try {
            root = json::parse(catalog.body);
        } catch (const Error& e) {
            if (log) log("catalog query '" + query + "' unparseable: " + std::string{e.what()});
            continue;
        }
        const json::Value* results = root.find("results");
        if (results == nullptr) continue;
        std::size_t sampled_here = 0;

        for (const json::Value& entry : results->items()) {
            const json::Value* resource = entry.find("resource");
            const json::Value* metadata = entry.find("metadata");
            if (resource == nullptr || metadata == nullptr) continue;
            const json::Value* id = resource->find("id");
            const json::Value* domain = metadata->find("domain");
            const json::Value* fields = resource->find("columns_field_name");
            const json::Value* names = resource->find("columns_name");
            if (id == nullptr || domain == nullptr || fields == nullptr || names == nullptr) {
                continue;
            }
            const std::string dataset_id = id->as_string();
            const std::string dataset_domain = domain->as_string();
            if (!seen_datasets.insert(dataset_domain + "/" + dataset_id).second) continue;
            ++local.datasets_seen;

            const fetch::Result rows_fetch =
                fetch::get("https://" + dataset_domain + "/resource/" + dataset_id +
                               ".json?$limit=" + std::to_string(options.sample_rows),
                           net);
            json::Value rows;
            if (rows_fetch.ok) {
                try {
                    rows = json::parse(rows_fetch.body);
                } catch (const Error&) {
                    rows = json::Value{};
                }
            }
            const bool has_rows = rows.is_array() && !rows.items().empty();
            if (has_rows) {
                ++local.datasets_sampled;
                ++sampled_here;
            }

            std::size_t none_kept = 0;
            for (std::size_t c = 0; c < fields->items().size(); ++c) {
                const std::string field_name = fields->items()[c].as_string();
                if (field_name.empty() || field_name[0] == ':') continue;
                const std::string display = c < names->items().size()
                                                ? names->items()[c].as_string()
                                                : field_name;
                columns::Example example;
                example.name = display.empty() ? field_name : display;
                example.domain = dataset_domain;
                if (has_rows) {
                    for (const json::Value& row : rows.items()) {
                        if (example.values.size() >= 3) break;
                        const json::Value* cell = row.find(field_name);
                        if (cell == nullptr) continue;
                        const std::string text = cell_text(*cell);
                        if (!text.empty()) example.values.push_back(text);
                    }
                }
                const std::string label = weak_label(registry, field_name, display);
                if (label.empty()) {
                    if (none_kept >= options.max_none_per_dataset) continue;
                    ++none_kept;
                    example.label = "none";
                    ++local.none;
                } else {
                    example.label = label;
                    ++local.labeled;
                }
                domains.insert(dataset_domain);
                out.push_back(std::move(example));
            }
        }
        if (log) {
            log("'" + query + "': " + std::to_string(sampled_here) + " datasets sampled, " +
                std::to_string(out.size()) + " columns so far");
        }
    }
    local.domains = domains.size();
    if (stats != nullptr) *stats = local;
    return out;
}

} // namespace dd::harvest
