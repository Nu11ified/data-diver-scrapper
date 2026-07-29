#include "dd/engine/store.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <filesystem>

namespace dd::store {
namespace {
std::string get_string(const json::Value& v, const char* key) {
    const json::Value* member = v.find(key);
    return member == nullptr ? std::string{} : member->as_string();
}

double get_number(const json::Value& v, const char* key) {
    const json::Value* member = v.find(key);
    return member == nullptr ? 0.0 : member->as_number();
}

bool get_bool(const json::Value& v, const char* key, bool fallback = false) {
    const json::Value* member = v.find(key);
    return member == nullptr ? fallback : member->as_bool(fallback);
}

std::string unique_id(std::string_view basis) {
    return str::hex64(str::hash64(std::string{basis} + "|" + timeutil::iso_now() + "|" +
                                  std::to_string(timeutil::unix_now())));
}
} // namespace

std::string RunRecord::serialize() const {
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.field("source_id", source_id);
    w.field("started_at", started_at);
    w.field("ok", ok);
    w.field("error", error);
    w.field("stage", stage);
    w.field("http_status", static_cast<std::int64_t>(http_status));
    w.field("bytes", bytes);
    w.field("fetch_ms", fetch_ms);
    w.field("parse_ms", parse_ms);
    w.field("classify_ms", classify_ms);
    w.field("map_ms", map_ms);
    w.field("total_ms", total_ms);
    w.field("format", format);
    w.field("classification", classification);
    w.field("class_confidence", class_confidence);
    w.field("records_extracted", records_extracted);
    w.field("events_new", events_new);
    w.field("newest_record_date", newest_record_date);
    w.field("extraction_rate", extraction_rate);
    w.field("mapping_confidence", mapping_confidence);
    w.field("baseline_rate", baseline_rate);
    w.field("drift_detected", drift_detected);
    w.field("repair_attempted", repair_attempted);
    w.field("repair_accepted", repair_accepted);
    w.field("structure_fingerprint", structure_fingerprint);
    w.field("rss_bytes", rss_bytes);
    w.field("cpu_ms", cpu_ms);
    w.end_object();
    return w.take();
}

RunRecord RunRecord::deserialize(const std::string& text) {
    const json::Value v = json::parse(text);
    RunRecord r;
    r.id = get_string(v, "id");
    r.source_id = get_string(v, "source_id");
    r.started_at = get_string(v, "started_at");
    r.ok = get_bool(v, "ok");
    r.error = get_string(v, "error");
    r.stage = get_string(v, "stage");
    r.http_status = static_cast<long>(get_number(v, "http_status"));
    r.bytes = static_cast<std::int64_t>(get_number(v, "bytes"));
    r.fetch_ms = get_number(v, "fetch_ms");
    r.parse_ms = get_number(v, "parse_ms");
    r.classify_ms = get_number(v, "classify_ms");
    r.map_ms = get_number(v, "map_ms");
    r.total_ms = get_number(v, "total_ms");
    r.format = get_string(v, "format");
    r.classification = get_string(v, "classification");
    r.class_confidence = get_number(v, "class_confidence");
    r.records_extracted = static_cast<std::int64_t>(get_number(v, "records_extracted"));
    r.events_new = static_cast<std::int64_t>(get_number(v, "events_new"));
    r.extraction_rate = get_number(v, "extraction_rate");
    r.mapping_confidence = get_number(v, "mapping_confidence");
    r.baseline_rate = get_number(v, "baseline_rate");
    r.drift_detected = get_bool(v, "drift_detected");
    r.repair_attempted = get_bool(v, "repair_attempted");
    r.repair_accepted = get_bool(v, "repair_accepted");
    r.structure_fingerprint = get_string(v, "structure_fingerprint");
    r.rss_bytes = static_cast<std::int64_t>(get_number(v, "rss_bytes"));
    r.cpu_ms = get_number(v, "cpu_ms");
    if (r.id.empty() || r.source_id.empty()) throw Error("store: run record missing id");
    return r;
}

std::string RepairRecord::serialize() const {
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.field("source_id", source_id);
    w.field("at", at);
    w.field("reason", reason);
    w.field_raw("before_mapping", before_mapping_json.empty() ? "null" : before_mapping_json);
    w.field_raw("after_mapping", after_mapping_json.empty() ? "null" : after_mapping_json);
    w.field("before_rate", before_rate);
    w.field("after_rate", after_rate);
    w.field("confidence", confidence);
    w.field("accepted", accepted);
    w.field("resolution", resolution);
    w.key("changes");
    w.begin_array();
    for (const std::string& change : changes) w.string_value(change);
    w.end_array();
    w.end_object();
    return w.take();
}

RepairRecord RepairRecord::deserialize(const std::string& text) {
    const json::Value v = json::parse(text);
    RepairRecord r;
    r.id = get_string(v, "id");
    r.source_id = get_string(v, "source_id");
    r.at = get_string(v, "at");
    r.reason = get_string(v, "reason");
    const json::Value* before = v.find("before_mapping");
    if (before != nullptr && !before->is_null()) r.before_mapping_json = before->serialize();
    const json::Value* after = v.find("after_mapping");
    if (after != nullptr && !after->is_null()) r.after_mapping_json = after->serialize();
    r.before_rate = get_number(v, "before_rate");
    r.after_rate = get_number(v, "after_rate");
    r.confidence = get_number(v, "confidence");
    r.accepted = get_bool(v, "accepted");
    const json::Value* resolution = v.find("resolution");
    r.resolution = resolution == nullptr ? (r.accepted ? "auto" : "pending")
                                         : resolution->as_string();
    const json::Value* changes = v.find("changes");
    if (changes != nullptr && changes->is_array()) {
        for (const json::Value& item : changes->items()) r.changes.push_back(item.as_string());
    }
    if (r.id.empty() || r.source_id.empty()) throw Error("store: repair record missing id");
    return r;
}

Store::Store(std::string root) : root_{std::move(root)} {
    fileio::ensure_dir(root_);
    fileio::ensure_dir(root_ + "/state");
    fileio::ensure_dir(root_ + "/records");
    fileio::ensure_dir(root_ + "/cache");
    fileio::ensure_dir(root_ + "/local");
    load();
}

void Store::load() {
    const std::lock_guard<std::mutex> lock{mutex_};

    const std::string sources_path = root_ + "/sources.json";
    if (fileio::exists(sources_path)) {
        const json::Value list = json::parse(fileio::read_file(sources_path));
        for (const json::Value& entry : list.items()) {
            Source s;
            s.id = get_string(entry, "id");
            s.name = get_string(entry, "name");
            s.url = get_string(entry, "url");
            s.jurisdiction = get_string(entry, "jurisdiction");
            s.added_at = get_string(entry, "added_at");
            s.enabled = get_bool(entry, "enabled", true);
            s.seed_from = get_string(entry, "seed_from");
            s.as_of = get_string(entry, "as_of");
            if (!s.id.empty() && !s.url.empty()) sources_.push_back(std::move(s));
        }
    }

    for (const std::string& line : fileio::read_lines(root_ + "/runs.jsonl")) {
        try {
            runs_.push_back(RunRecord::deserialize(line));
        } catch (const Error& e) {
            logging::warn(std::string{"store: skipping bad run record: "} + e.what());
        }
    }
    for (const std::string& line : fileio::read_lines(root_ + "/repairs.jsonl")) {
        try {
            repairs_.push_back(RepairRecord::deserialize(line));
        } catch (const Error& e) {
            logging::warn(std::string{"store: skipping bad repair record: "} + e.what());
        }
    }
    for (const std::string& line : fileio::read_lines(root_ + "/events.jsonl")) {
        try {
            events::PropertyEvent e = events::PropertyEvent::deserialize(line);
            if (event_index_.find(e.id) != event_index_.end()) continue;
            event_index_[e.id] = events_.size();
            events_.push_back(std::move(e));
        } catch (const Error& e) {
            logging::warn(std::string{"store: skipping bad event: "} + e.what());
        }
    }

    for (const std::string& path : fileio::list_dir(root_ + "/state")) {
        try {
            const json::Value v = json::parse(fileio::read_file(path));
            SourceState state;
            state.source_id = get_string(v, "source_id");
            if (state.source_id.empty()) continue;
            state.fingerprint = get_string(v, "fingerprint");
            state.baseline_rate = get_number(v, "baseline_rate");
            state.good_runs = static_cast<int>(get_number(v, "good_runs"));
            state.classification = get_string(v, "classification");
            state.updated_at = get_string(v, "updated_at");
            const json::Value* mapping = v.find("mapping");
            if (mapping != nullptr && !mapping->is_null()) {
                state.mapping = schema::Mapping::deserialize(mapping->serialize());
                state.has_mapping = !state.mapping.fields.empty();
            }
            const json::Value* overrides = v.find("overrides");
            if (overrides != nullptr && overrides->is_object()) {
                for (const auto& [field, label] : overrides->members()) {
                    state.overrides[field] = label.as_string();
                }
            }
            states_[state.source_id] = std::move(state);
        } catch (const Error& e) {
            logging::warn(std::string{"store: skipping bad source state: "} + e.what());
        }
    }
}

void Store::persist_sources_locked() {
    json::Writer w;
    w.begin_array();
    for (const Source& s : sources_) {
        w.begin_object();
        w.field("id", s.id);
        w.field("name", s.name);
        w.field("url", s.url);
        w.field("jurisdiction", s.jurisdiction);
        w.field("added_at", s.added_at);
        w.field("enabled", s.enabled);
        w.field("seed_from", s.seed_from);
        w.field("as_of", s.as_of);
        w.end_object();
    }
    w.end_array();
    fileio::write_file_atomic(root_ + "/sources.json", w.str());
}

void Store::seed(const std::string& seeds_path) {
    const std::lock_guard<std::mutex> lock{mutex_};
    if (!fileio::exists(seeds_path)) return;
    bool changed = false;
    const json::Value list = json::parse(fileio::read_file(seeds_path));
    for (const json::Value& entry : list.items()) {
        Source s;
        s.id = get_string(entry, "id");
        s.name = get_string(entry, "name");
        s.url = get_string(entry, "url");
        s.jurisdiction = get_string(entry, "jurisdiction");
        s.seed_from = get_string(entry, "seed_from");
        s.as_of = get_string(entry, "as_of");
        s.added_at = timeutil::iso_now();
        if (s.id.empty() || s.url.empty()) continue;
        const bool present = std::any_of(sources_.begin(), sources_.end(),
                                         [&](const Source& e) { return e.id == s.id; });
        if (!present) {
            sources_.push_back(std::move(s));
            changed = true;
        }
    }
    if (changed) persist_sources_locked();
    for (const Source& s : sources_) {
        if (s.seed_from.empty() || fileio::exists(s.url)) continue;
        if (!fileio::exists(s.seed_from)) {
            logging::warn("store: seed file missing: " + s.seed_from);
            continue;
        }
        fileio::write_file_atomic(s.url, fileio::read_file(s.seed_from));
        logging::info("store: seeded " + s.url + " from " + s.seed_from);
    }
}

std::vector<Source> Store::sources() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    return sources_;
}

std::optional<Source> Store::find_source(const std::string& id) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    for (const Source& s : sources_) {
        if (s.id == id) return s;
    }
    return std::nullopt;
}

Source Store::add_source(const std::string& name, const std::string& url,
                         const std::string& jurisdiction) {
    if (str::trim(name).empty() || str::trim(url).empty()) {
        throw Error("store: source needs a name and a url");
    }
    const std::lock_guard<std::mutex> lock{mutex_};
    Source s;
    s.id = str::slug(name) + "_" + unique_id(url).substr(0, 6);
    s.name = str::trim(name);
    s.url = str::trim(url);
    s.jurisdiction = str::trim(jurisdiction).empty() ? s.name : str::trim(jurisdiction);
    s.added_at = timeutil::iso_now();
    sources_.push_back(s);
    persist_sources_locked();
    return s;
}

Source Store::update_source(const std::string& id, const SourceUpdate& update) {
    const std::lock_guard<std::mutex> lock{mutex_};
    for (Source& s : sources_) {
        if (s.id != id) continue;
        if (update.name.has_value()) {
            if (str::trim(*update.name).empty()) throw Error("store: source name cannot be empty");
            s.name = str::trim(*update.name);
        }
        if (update.url.has_value()) {
            if (str::trim(*update.url).empty()) throw Error("store: source url cannot be empty");
            s.url = str::trim(*update.url);
        }
        if (update.jurisdiction.has_value()) s.jurisdiction = str::trim(*update.jurisdiction);
        if (update.enabled.has_value()) s.enabled = *update.enabled;
        persist_sources_locked();
        return s;
    }
    throw Error("store: unknown source: " + id);
}

void Store::remove_source(const std::string& id) {
    const std::lock_guard<std::mutex> lock{mutex_};
    const auto it = std::find_if(sources_.begin(), sources_.end(),
                                 [&](const Source& s) { return s.id == id; });
    if (it == sources_.end()) throw Error("store: unknown source: " + id);
    sources_.erase(it);
    persist_sources_locked();
    states_.erase(id);
    std::error_code ec; // best effort: a missing file is already gone
    std::filesystem::remove(state_path(id), ec);
    std::filesystem::remove(root_ + "/records/" + id + ".json", ec);
    std::filesystem::remove(cache_path(id), ec);
    std::filesystem::remove(cache_path(id) + ".meta", ec);
}

std::string Store::state_path(const std::string& source_id) const {
    return root_ + "/state/" + source_id + ".json";
}

std::string Store::cache_path(const std::string& source_id) const {
    return root_ + "/cache/" + source_id;
}

SourceState Store::source_state(const std::string& source_id) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    const auto it = states_.find(source_id);
    if (it != states_.end()) return it->second;
    SourceState empty;
    empty.source_id = source_id;
    return empty;
}

void Store::save_source_state(const SourceState& state) {
    const std::lock_guard<std::mutex> lock{mutex_};
    save_source_state_locked(state);
}

void Store::save_source_state_locked(const SourceState& state) {
    json::Writer w;
    w.begin_object();
    w.field("source_id", state.source_id);
    w.field("fingerprint", state.fingerprint);
    w.field("baseline_rate", state.baseline_rate);
    w.field("good_runs", state.good_runs);
    w.field("classification", state.classification);
    w.field("updated_at", state.updated_at);
    if (state.has_mapping) {
        w.field_raw("mapping", state.mapping.serialize());
    } else {
        w.key("mapping");
        w.null_value();
    }
    w.key("overrides");
    w.begin_object();
    for (const auto& [field, label] : state.overrides) w.field(field, label);
    w.end_object();
    w.end_object();
    fileio::write_file_atomic(state_path(state.source_id), w.str());
    states_[state.source_id] = state;
}

void Store::record_run(const RunRecord& run) {
    const std::lock_guard<std::mutex> lock{mutex_};
    fileio::append_line(root_ + "/runs.jsonl", run.serialize());
    runs_.push_back(run);
}

std::vector<RunRecord> Store::runs(std::size_t limit, const std::string& source_id) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::vector<RunRecord> out;
    for (auto it = runs_.rbegin(); it != runs_.rend() && out.size() < limit; ++it) {
        if (!source_id.empty() && it->source_id != source_id) continue;
        out.push_back(*it);
    }
    return out;
}

std::size_t Store::add_events(const std::vector<events::PropertyEvent>& batch) {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::size_t added = 0;
    for (const events::PropertyEvent& e : batch) {
        if (e.id.empty() || e.property_key.empty()) continue;
        if (event_index_.find(e.id) != event_index_.end()) continue;
        fileio::append_line(root_ + "/events.jsonl", e.serialize());
        event_index_[e.id] = events_.size();
        events_.push_back(e);
        ++added;
    }
    return added;
}

std::vector<events::PropertyEvent> Store::events_for(const std::string& property_key) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::vector<events::PropertyEvent> out;
    for (const events::PropertyEvent& e : events_) {
        if (e.property_key == property_key) out.push_back(e);
    }
    return out;
}

std::vector<events::PropertyEvent> Store::all_events() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    return events_;
}

std::vector<std::string> Store::property_keys() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::vector<std::string> keys;
    for (const events::PropertyEvent& e : events_) {
        if (std::find(keys.begin(), keys.end(), e.property_key) == keys.end()) {
            keys.push_back(e.property_key);
        }
    }
    return keys;
}

std::size_t Store::event_count() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    return events_.size();
}

void Store::save_latest_records(const std::string& source_id, const std::string& records_json) {
    const std::lock_guard<std::mutex> lock{mutex_};
    fileio::write_file_atomic(root_ + "/records/" + source_id + ".json", records_json);
}

std::string Store::latest_records(const std::string& source_id) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    const std::string path = root_ + "/records/" + source_id + ".json";
    if (!fileio::exists(path)) return "[]";
    return fileio::read_file(path);
}

void Store::save_fetch_cache(const std::string& source_id, const std::string& content_type,
                             const std::string& body) {
    const std::lock_guard<std::mutex> lock{mutex_};
    fileio::write_file_atomic(cache_path(source_id), body);
    json::Writer w;
    w.begin_object();
    w.field("content_type", content_type);
    w.field("fetched_at", timeutil::iso_now());
    w.field("bytes", static_cast<std::int64_t>(body.size()));
    w.end_object();
    fileio::write_file_atomic(cache_path(source_id) + ".meta", w.str());
}

std::optional<CachedFetch> Store::fetch_cache(const std::string& source_id) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    const std::string path = cache_path(source_id);
    if (!fileio::exists(path)) return std::nullopt;
    CachedFetch cached;
    cached.body = fileio::read_file(path);
    if (fileio::exists(path + ".meta")) {
        const json::Value meta = json::parse(fileio::read_file(path + ".meta"));
        cached.content_type = get_string(meta, "content_type");
    }
    return cached;
}

bool Store::has_fetch_cache(const std::string& source_id) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    return fileio::exists(cache_path(source_id));
}

void Store::add_repair(const RepairRecord& repair) {
    const std::lock_guard<std::mutex> lock{mutex_};
    fileio::append_line(root_ + "/repairs.jsonl", repair.serialize());
    repairs_.push_back(repair);
}

std::vector<RepairRecord> Store::repairs(const std::string& source_id) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::vector<RepairRecord> out;
    for (auto it = repairs_.rbegin(); it != repairs_.rend(); ++it) {
        if (!source_id.empty() && it->source_id != source_id) continue;
        out.push_back(*it);
    }
    return out;
}

RepairRecord Store::resolve_repair(const std::string& id, bool approved) {
    const std::lock_guard<std::mutex> lock{mutex_};
    const auto it = std::find_if(repairs_.begin(), repairs_.end(),
                                 [&](const RepairRecord& r) { return r.id == id; });
    if (it == repairs_.end()) throw Error("store: unknown repair: " + id);
    if (it->resolution != "pending") {
        throw Error("store: repair " + id + " is already resolved (" + it->resolution + ")");
    }
    it->resolution = approved ? "approved" : "rejected";

    std::string lines;
    for (const RepairRecord& r : repairs_) lines += r.serialize() + "\n";
    fileio::write_file_atomic(root_ + "/repairs.jsonl", lines);

    if (approved && !it->after_mapping_json.empty()) {
        SourceState state;
        const auto found = states_.find(it->source_id);
        if (found != states_.end()) state = found->second;
        state.source_id = it->source_id;
        state.mapping = schema::Mapping::deserialize(it->after_mapping_json);
        state.has_mapping = !state.mapping.fields.empty();
        state.good_runs = 0;
        state.updated_at = timeutil::iso_now();
        save_source_state_locked(state);
    }
    return *it;
}
} // namespace dd::store
