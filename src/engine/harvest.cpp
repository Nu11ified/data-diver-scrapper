#include "dd/engine/harvest.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"
#include "dd/net/fetch.hpp"

#include <set>

namespace dd::harvest {
namespace {
constexpr const char* kCatalog = "https://api.us.socrata.com/api/catalog/v1";
constexpr const char* kArcgisHub = "https://hub.arcgis.com/api/v3/datasets";

// CKAN is per-portal: data.gov's federated search stopped answering, so the
// portals that do are listed rather than discovered.
const std::vector<std::string>& ckan_portals() {
    static const std::vector<std::string> portals = {"data.ca.gov", "data.boston.gov"};
    return portals;
}

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

namespace {
// An ArcGIS layer answers rows only through a query call; the catalog hands
// back the service root, so the layer index and query string are appended.
std::string arcgis_rows_url(const std::string& service_url) {
    std::string base = service_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.size() < 8) return {};
    const std::size_t slash = base.rfind('/');
    const bool ends_with_layer =
        slash != std::string::npos && slash + 1 < base.size() &&
        base.find_first_not_of("0123456789", slash + 1) == std::string::npos;
    if (!ends_with_layer) {
        if (!str::contains(str::to_lower(base), "featureserver")) return {};
        base += "/0";
    }
    return base + "/query?where=1%3D1&outFields=*&f=json&resultRecordCount=50";
}

void discover_arcgis(const std::vector<std::string>& queries, std::size_t per_query,
                     const fetch::Options& net, std::set<std::string>& seen,
                     std::vector<Discovered>& out,
                     const std::function<void(const std::string&)>& log) {
    for (const std::string& query : queries) {
        const std::string url = std::string{kArcgisHub} + "?q=" + url_encode(query) +
                                "&filter%5Btype%5D=" + url_encode("Feature Service") +
                                "&page%5Bsize%5D=" + std::to_string(per_query);
        const fetch::Result response = fetch::get(url, net);
        if (!response.ok) {
            if (log) log("arcgis '" + query + "' failed: " + response.error);
            continue;
        }
        json::Value root;
        try {
            root = json::parse(response.body);
        } catch (const Error&) {
            continue;
        }
        const json::Value* data = root.find("data");
        if (data == nullptr) continue;
        std::size_t taken = 0;
        for (const json::Value& entry : data->items()) {
            if (taken >= per_query) break;
            const json::Value* id = entry.find("id");
            const json::Value* attributes = entry.find("attributes");
            if (id == nullptr || attributes == nullptr) continue;
            const json::Value* name = attributes->find("name");
            const json::Value* service = attributes->find("url");
            if (name == nullptr || service == nullptr) continue;
            const std::string rows = arcgis_rows_url(service->as_string());
            if (rows.empty()) continue;
            const std::string dataset_id = id->as_string();
            if (dataset_id.empty() || !seen.insert("arcgis:" + dataset_id).second) continue;

            const json::Value* source = attributes->find("source");
            const json::Value* owner = attributes->find("owner");
            std::string jurisdiction = source == nullptr ? std::string{} : source->as_string();
            if (jurisdiction.empty() && owner != nullptr) jurisdiction = owner->as_string();
            if (jurisdiction.empty()) jurisdiction = "ArcGIS Hub";

            Discovered d;
            d.id = "arcgis_" + str::replace_all(dataset_id, "-", "_");
            d.name = name->as_string();
            d.url = rows;
            d.jurisdiction = jurisdiction;
            d.query = query;
            out.push_back(std::move(d));
            ++taken;
        }
        if (log) log("arcgis '" + query + "': " + std::to_string(taken) + " sources");
    }
}

void discover_ckan(const std::vector<std::string>& queries, std::size_t per_query,
                   const fetch::Options& net, std::set<std::string>& seen,
                   std::vector<Discovered>& out,
                   const std::function<void(const std::string&)>& log) {
    for (const std::string& portal : ckan_portals()) {
        if (!plausible_domain(portal)) continue;
        std::size_t taken = 0;
        for (const std::string& query : queries) {
            const std::string url = "https://" + portal +
                                    "/api/3/action/package_search?rows=" +
                                    std::to_string(per_query) + "&q=" + url_encode(query);
            const fetch::Result response = fetch::get(url, net);
            if (!response.ok) continue;
            json::Value root;
            try {
                root = json::parse(response.body);
            } catch (const Error&) {
                continue;
            }
            const json::Value* result = root.find("result");
            if (result == nullptr) continue;
            const json::Value* packages = result->find("results");
            if (packages == nullptr) continue;
            for (const json::Value& package : packages->items()) {
                const json::Value* title = package.find("title");
                const json::Value* resources = package.find("resources");
                if (title == nullptr || resources == nullptr) continue;
                for (const json::Value& resource : resources->items()) {
                    const json::Value* format = resource.find("format");
                    const json::Value* link = resource.find("url");
                    const json::Value* resource_id = resource.find("id");
                    if (format == nullptr || link == nullptr || resource_id == nullptr) continue;
                    const std::string kind = str::to_lower(format->as_string());
                    if (kind != "csv" && kind != "json" && kind != "geojson") continue;
                    const std::string href = link->as_string();
                    if (href.rfind("https://", 0) != 0) continue;
                    if (!seen.insert("ckan:" + resource_id->as_string()).second) continue;

                    const json::Value* organization = package.find("organization");
                    const json::Value* org_title =
                        organization == nullptr ? nullptr : organization->find("title");

                    Discovered d;
                    d.id = "ckan_" + str::slug(portal) + "_" +
                           str::replace_all(resource_id->as_string(), "-", "_");
                    d.name = title->as_string();
                    d.url = href;
                    d.jurisdiction =
                        org_title == nullptr ? portal : org_title->as_string();
                    d.query = query;
                    out.push_back(std::move(d));
                    ++taken;
                    break; // one resource per package is enough to try
                }
            }
        }
        if (log) log("ckan " + portal + ": " + std::to_string(taken) + " sources");
    }
}
} // namespace

std::vector<Discovered> discover(std::size_t datasets_per_query,
                                 const std::function<void(const std::string&)>& log) {
    fetch::Options net;
    net.timeout_seconds = 20;
    std::vector<Discovered> out;
    std::set<std::string> seen;
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
        if (log) log("socrata '" + query + "': " + std::to_string(taken) + " sources");
    }
    discover_arcgis(kQueries, datasets_per_query, net, seen, out, log);
    discover_ckan(kQueries, datasets_per_query, net, seen, out, log);
    return out;
}
} // namespace dd::harvest
