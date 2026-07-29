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

namespace {

std::string exact_hit(const schema::Registry& registry, const std::string& slug) {
    if (slug.empty()) return {};
    for (const schema::FieldDef& field : registry.fields()) {
        if (str::slug(field.name) == slug) return field.name;
        for (const std::string& synonym : field.synonyms) {
            if (str::slug(synonym) == slug) return field.name;
        }
    }
    return {};
}

bool touches_lexicon(const schema::Registry& registry, const std::string& slug) {
    if (slug.size() < 4) return false;
    for (const schema::FieldDef& field : registry.fields()) {
        std::vector<std::string> vocabulary{str::slug(field.name)};
        for (const std::string& synonym : field.synonyms) vocabulary.push_back(str::slug(synonym));
        for (const std::string& word : vocabulary) {
            if (word.size() < 4) continue;
            if (str::contains(slug, word) || str::contains(word, slug)) return true;
        }
    }
    return false;
}

bool plausible_domain(const std::string& domain) {
    if (domain.empty() || domain.size() > 128 || !str::contains(domain, ".")) return false;
    if (domain == "localhost" || domain.front() == '.' || domain.back() == '.') return false;
    bool any_alpha = false;
    for (char c : domain) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) return false;
        if (c >= 'a' && c <= 'z') any_alpha = true;
    }
    return any_alpha; // an all-digits-and-dots "domain" is a raw IP: refuse
}

bool plausible_dataset_id(const std::string& id) {
    if (id.size() != 9 || id[4] != '-') return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 4) continue;
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

} // namespace

WeakLabel weak_label(const schema::Registry& registry, const std::string& field_name,
                     const std::string& display_name) {
    const std::string field_slug = str::slug(field_name);
    const std::string display_slug = str::slug(display_name);
    const std::string by_field = exact_hit(registry, field_slug);
    const std::string by_display = exact_hit(registry, display_slug);
    if (!by_field.empty() && !by_display.empty() && by_field != by_display) {
        return WeakLabel{"", true}; // the two names disagree: ambiguous
    }
    if (!by_field.empty()) return WeakLabel{by_field, false};
    if (!by_display.empty()) return WeakLabel{by_display, false};
    if (touches_lexicon(registry, field_slug) || touches_lexicon(registry, display_slug)) {
        return WeakLabel{"", true}; // near miss: probably a positive we cannot verify
    }
    return WeakLabel{"", false};
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
        std::size_t entries_taken = 0;

        for (const json::Value& entry : results->items()) {
            if (++entries_taken > options.datasets_per_query) break;
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
            if (!plausible_domain(dataset_domain) || !plausible_dataset_id(dataset_id)) continue;
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
            bool has_rows = false;
            if (rows.is_array()) {
                for (const json::Value& row : rows.items()) {
                    if (!row.is_object()) continue;
                    for (const auto& [key, cell] : row.members()) {
                        if (!cell_text(cell).empty()) {
                            has_rows = true;
                            break;
                        }
                    }
                    if (has_rows) break;
                }
            }
            if (!has_rows) continue; // metadata without values teaches nothing safe
            ++local.datasets_sampled;
            ++sampled_here;

            std::size_t none_kept = 0;
            const std::size_t column_cap = std::min<std::size_t>(fields->items().size(), 64);
            for (std::size_t c = 0; c < column_cap; ++c) {
                const std::string field_name = fields->items()[c].as_string();
                if (field_name.empty() || field_name[0] == ':') continue;
                const std::string display = c < names->items().size()
                                                ? names->items()[c].as_string()
                                                : field_name;
                columns::Example example;
                example.name = display.empty() ? field_name : display;
                example.domain = dataset_domain;
                for (const json::Value& row : rows.items()) {
                    if (example.values.size() >= 3) break;
                    if (!row.is_object()) continue;
                    const json::Value* cell = row.find(field_name);
                    if (cell == nullptr) continue;
                    const std::string text = cell_text(*cell);
                    if (!text.empty()) example.values.push_back(text);
                }
                const WeakLabel label = weak_label(registry, field_name, display);
                if (label.masked) {
                    ++local.masked;
                    continue;
                }
                if (label.field.empty()) {
                    if (none_kept >= options.max_none_per_dataset) continue;
                    ++none_kept;
                    example.label = "none";
                    ++local.none;
                } else {
                    example.label = label.field;
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

std::map<std::string, std::size_t> grow_corpus(
    const std::string& corpus_dir, std::size_t datasets_per_query,
    const std::function<void(const std::string&)>& log) {
    static const std::vector<std::pair<const char*, std::vector<const char*>>> kQueries = {
        {"tax_delinquency", {"delinquent property tax", "tax delinquent list"}},
        {"building_permit", {"building permits issued", "construction permits"}},
        {"code_violation", {"code enforcement violations", "property maintenance violations"}},
        {"deed_transfer", {"real property sales deed", "property transfers"}},
        {"foreclosure_filing", {"foreclosure filings", "lis pendens"}},
        {"trustee_auction", {"sheriff sale", "tax sale results"}},
        {"assessor_roll", {"property assessment roll", "parcel assessments"}},
        {"probate_case", {"probate court", "estate cases"}},
    };
    fetch::Options net;
    net.timeout_seconds = 20;
    std::map<std::string, std::size_t> written;
    std::set<std::string> seen_datasets;

    for (const auto& [label, queries] : kQueries) {
        for (const char* query : queries) {
            const std::string url = std::string{kCatalog} + "?only=datasets&limit=" +
                                    std::to_string(datasets_per_query) +
                                    "&q=" + url_encode(query);
            const fetch::Result catalog = fetch::get(url, net);
            if (!catalog.ok) {
                if (log) log(std::string{"catalog '"} + query + "' failed: " + catalog.error);
                continue;
            }
            json::Value root;
            try {
                root = json::parse(catalog.body);
            } catch (const Error&) {
                continue;
            }
            const json::Value* results = root.find("results");
            if (results == nullptr) continue;
            std::size_t taken = 0;
            for (const json::Value& entry : results->items()) {
                if (taken >= datasets_per_query) break;
                const json::Value* resource = entry.find("resource");
                const json::Value* metadata = entry.find("metadata");
                if (resource == nullptr || metadata == nullptr) continue;
                const json::Value* id = resource->find("id");
                const json::Value* domain = metadata->find("domain");
                const json::Value* fields = resource->find("columns_field_name");
                if (id == nullptr || domain == nullptr || fields == nullptr) continue;
                const std::string dataset_id = id->as_string();
                const std::string dataset_domain = domain->as_string();
                if (!plausible_domain(dataset_domain) || !plausible_dataset_id(dataset_id)) {
                    continue;
                }
                if (fields->items().size() < 4) continue; // too thin to classify
                if (!seen_datasets.insert(dataset_id).second) continue;
                const std::string path = corpus_dir + "/" + label + "/socrata_" +
                                         str::replace_all(dataset_id, "-", "_") + ".json";
                if (fileio::exists(path)) continue;
                const fetch::Result rows = fetch::get(
                    "https://" + dataset_domain + "/resource/" + dataset_id + ".json?$limit=5",
                    net);
                if (!rows.ok || rows.body.size() < 64) continue;
                try {
                    const json::Value parsed = json::parse(rows.body);
                    if (!parsed.is_array() || parsed.items().empty() ||
                        !parsed.items().front().is_object()) {
                        continue;
                    }
                } catch (const Error&) {
                    continue;
                }
                fileio::write_file_atomic(path, rows.body);
                ++written[label];
                ++taken;
            }
        }
        if (log) {
            log(std::string{label} + ": " + std::to_string(written[label]) + " real datasets");
        }
    }
    return written;
}

std::vector<Discovered> discover(std::size_t datasets_per_query,
                                 const std::function<void(const std::string&)>& log) {
    fetch::Options net;
    net.timeout_seconds = 20;
    std::vector<Discovered> out;
    std::set<std::string> seen;
    // Property queries only: the column corpus needs unrelated datasets for
    // negatives, a source catalog does not.
    static const std::vector<std::string> kQueries = {
        "property tax", "tax sale", "delinquent", "code violations", "building permits",
        "property assessment", "foreclosure", "sheriff sale", "deed", "parcel",
        "housing inspection",
    };
    for (const std::string& query : kQueries) {
        const std::string url = std::string{kCatalog} + "?only=datasets&limit=" +
                                std::to_string(datasets_per_query) +
                                "&q=" + url_encode(query);
        const fetch::Result catalog = fetch::get(url, net);
        if (!catalog.ok) {
            if (log) log("catalog '" + query + "' failed: " + catalog.error);
            continue;
        }
        json::Value root;
        try {
            root = json::parse(catalog.body);
        } catch (const Error&) {
            continue;
        }
        const json::Value* results = root.find("results");
        if (results == nullptr) continue;
        std::size_t taken = 0;
        for (const json::Value& entry : results->items()) {
            if (taken >= datasets_per_query) break;
            const json::Value* resource = entry.find("resource");
            const json::Value* metadata = entry.find("metadata");
            if (resource == nullptr || metadata == nullptr) continue;
            const json::Value* id = resource->find("id");
            const json::Value* domain = metadata->find("domain");
            const json::Value* name = resource->find("name");
            const json::Value* fields = resource->find("columns_field_name");
            if (id == nullptr || domain == nullptr || name == nullptr || fields == nullptr) {
                continue;
            }
            const std::string dataset_id = id->as_string();
            const std::string dataset_domain = domain->as_string();
            if (!plausible_domain(dataset_domain) || !plausible_dataset_id(dataset_id)) continue;
            if (fields->items().size() < 4) continue;
            if (!seen.insert(dataset_id).second) continue;

            const json::Value* attribution = resource->find("attribution");
            std::string jurisdiction =
                attribution == nullptr ? std::string{} : attribution->as_string();
            if (jurisdiction.empty()) jurisdiction = dataset_domain;

            Discovered d;
            d.id = "socrata_" + str::replace_all(dataset_id, "-", "_");
            d.name = name->as_string();
            d.url = "https://" + dataset_domain + "/resource/" + dataset_id + ".json?$limit=50";
            d.jurisdiction = jurisdiction;
            d.query = query;
            out.push_back(std::move(d));
            ++taken;
        }
        if (log) log("'" + query + "': " + std::to_string(taken) + " sources");
    }
    return out;
}

} // namespace dd::harvest
