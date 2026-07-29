#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dd::doc {
enum class Format { Html, Json, Csv, Pdf, Text, Unknown };

std::string_view format_name(Format f);

Format detect_format(std::string_view content_type, std::string_view body);

struct Cell {
    std::string label;
    std::string value;
    std::string path;
};

struct RawRecord {
    std::vector<Cell> cells;

    const Cell* find(std::string_view label) const;
};

struct Model {
    Format format = Format::Unknown;
    std::string title;
    std::vector<std::string> headings;
    std::string text;                  // full visible text, for classification
    std::vector<std::string> labels;   // distinct cell labels in first-seen order
    std::vector<RawRecord> records;
    std::string container_signature;   // tag.class path of the repeated block, "" otherwise

    std::string structure_fingerprint() const;
};

Model build(Format format, std::string_view body);
Model build_auto(std::string_view content_type, std::string_view body);
} // namespace dd::doc
