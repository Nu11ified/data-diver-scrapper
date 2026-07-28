#pragma once

#include "dd/events.hpp"
#include "dd/schema.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dd::store {

struct Source {
    std::string id;
    std::string name;
    std::string url;
    std::string jurisdiction;
    std::string added_at;
    bool enabled = true;
    // When set, the file at `url` is created from this path if missing. Lets
    // shipped demo sources live under var/ where their bytes can change the
    // way a county site's bytes change.
    std::string seed_from;
};

// What the engine has learned about one source: its accepted mapping, the
// structural fingerprint of the last good document, and the extraction-rate
// baseline drift detection compares against.
struct SourceState {
    std::string source_id;
    bool has_mapping = false;
    schema::Mapping mapping;
    std::string fingerprint;
    double baseline_rate = 0.0; // exponential moving average of good runs
    int good_runs = 0;
    std::string classification;
    std::string updated_at;
};

// One ingestion attempt. Every number here is measured: timings from a
// monotonic clock, bytes from the fetcher, memory from the OS, confidences
// from the classifier and mapping scorer.
struct RunRecord {
    std::string id;
    std::string source_id;
    std::string started_at;
    bool ok = false;
    std::string error;
    std::string stage; // stage reached or failed: fetch/parse/classify/map/resolve/done

    long http_status = 0;
    std::int64_t bytes = 0;
    double fetch_ms = 0.0;
    double parse_ms = 0.0;
    double classify_ms = 0.0;
    double map_ms = 0.0;
    double total_ms = 0.0;

    std::string format;
    std::string classification;
    double class_confidence = 0.0;

    std::int64_t records_extracted = 0;
    std::int64_t events_new = 0;
    double extraction_rate = 0.0;
    double mapping_confidence = 0.0;
    double baseline_rate = 0.0;
    bool drift_detected = false;
    bool repair_attempted = false;
    bool repair_accepted = false;
    std::string structure_fingerprint;

    std::int64_t rss_bytes = 0;
    double cpu_ms = 0.0;

    std::string serialize() const;
    static RunRecord deserialize(const std::string& text);
};

// A mapping repair, kept with its before and after so the UI can show what
// the healer actually changed and on what evidence.
struct RepairRecord {
    std::string id;
    std::string source_id;
    std::string at;
    std::string reason;
    std::string before_mapping_json;
    std::string after_mapping_json;
    double before_rate = 0.0;
    double after_rate = 0.0;
    double confidence = 0.0;
    bool accepted = false; // false = queued for human review
    std::vector<std::string> changes; // "owner: 'Owner Name' -> 'taxpayer'"

    std::string serialize() const;
    static RepairRecord deserialize(const std::string& text);
};

// File-backed state under one root directory. Runs, events and repairs are
// append-only JSONL; sources and per-source state are small JSON documents
// written atomically. All access is serialized internally.
class Store {
public:
    explicit Store(std::string root);

    // Loads the prefilled source list when no sources exist yet, and creates
    // any seed_from working copies that are missing.
    void seed(const std::string& seeds_path);

    std::vector<Source> sources() const;
    std::optional<Source> find_source(const std::string& id) const;
    Source add_source(const std::string& name, const std::string& url,
                      const std::string& jurisdiction);

    SourceState source_state(const std::string& source_id) const;
    void save_source_state(const SourceState& state);

    void record_run(const RunRecord& run);
    std::vector<RunRecord> runs(std::size_t limit, const std::string& source_id = "") const;

    // Returns how many events were new; already-known ids are skipped, which
    // is what makes re-ingestion idempotent.
    std::size_t add_events(const std::vector<events::PropertyEvent>& batch);
    std::vector<events::PropertyEvent> events_for(const std::string& property_key) const;
    std::vector<std::string> property_keys() const;
    std::size_t event_count() const;

    void save_latest_records(const std::string& source_id, const std::string& records_json);
    std::string latest_records(const std::string& source_id) const;

    void add_repair(const RepairRecord& repair);
    std::vector<RepairRecord> repairs(const std::string& source_id = "") const;

    const std::string& root() const noexcept { return root_; }

private:
    void load();
    void persist_sources_locked();
    std::string state_path(const std::string& source_id) const;

    std::string root_;
    mutable std::mutex mutex_;
    std::vector<Source> sources_;
    std::map<std::string, SourceState> states_;
    std::vector<RunRecord> runs_;
    std::vector<RepairRecord> repairs_;
    std::vector<events::PropertyEvent> events_;
    std::map<std::string, std::size_t> event_index_; // id -> position
};

} // namespace dd::store
