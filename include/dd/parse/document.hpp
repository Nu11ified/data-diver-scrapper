#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dd::doc {

enum class Format { Html, Json, Csv, Pdf, Text, Unknown };

std::string_view format_name(Format f);

// Sniffs bytes first, then falls back on the transport content type. County
// servers frequently mislabel exports, so the bytes win disagreements.
Format detect_format(std::string_view content_type, std::string_view body);

// One extracted datum with its provenance inside the source document. The
// label is whatever the source called it (header cell, JSON key, CSS class,
// data attribute); mapping those dialect labels onto canonical fields is the
// schema stage's job, not extraction's.
struct Cell {
    std::string label;
    std::string value;
    std::string path;
};

struct RawRecord {
    std::vector<Cell> cells;

    const Cell* find(std::string_view label) const;
};

// Uniform view of a source document for every downstream stage.
struct Model {
    Format format = Format::Unknown;
    std::string title;
    std::vector<std::string> headings;
    std::string text;                  // full visible text, for classification
    std::vector<std::string> labels;   // distinct cell labels in first-seen order
    std::vector<RawRecord> records;
    std::string container_signature;   // tag.class path of the repeated block, "" otherwise

    // Stable hash of format, labels and container shape. Changes when the
    // source changes shape, which is what drift detection watches.
    std::string structure_fingerprint() const;
};

// Never throws for supported formats; a document with no recognizable records
// yields records.empty() and downstream stages report that honestly.
Model build(Format format, std::string_view body);
Model build_auto(std::string_view content_type, std::string_view body);

} // namespace dd::doc
