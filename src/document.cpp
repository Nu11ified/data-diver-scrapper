#include "dd/document.hpp"

#include "dd/core.hpp"
#include "dd/csv.hpp"
#include "dd/html.hpp"
#include "dd/json.hpp"
#include "dd/pdf.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>

namespace dd::doc {
namespace {

// ------------------------------------------------------------- helpers -----

bool label_is_generic(std::string_view label) {
    static const std::vector<std::string> kGeneric = {
        "value", "cell",  "item", "field", "row",  "col",   "column", "data",
        "text",  "label", "main", "inner", "left", "right", "entry",  "record"};
    const std::string lowered = str::to_lower(label);
    return std::find(kGeneric.begin(), kGeneric.end(), lowered) != kGeneric.end();
}

std::string class_label(const html::Node& node) {
    std::vector<std::string> parts;
    for (const std::string& c : node.classes()) {
        if (!label_is_generic(c)) parts.push_back(c);
    }
    return str::join(parts, " ");
}

void add_label(std::vector<std::string>& labels, const std::string& label) {
    if (label.empty()) return;
    if (std::find(labels.begin(), labels.end(), label) == labels.end()) labels.push_back(label);
}

void finish(Model& model) {
    for (const RawRecord& record : model.records) {
        for (const Cell& cell : record.cells) add_label(model.labels, cell.label);
    }
}

bool cell_text_is_numericish(std::string_view s) {
    const std::string cleaned = str::trim(s);
    if (cleaned.empty()) return false;
    std::size_t digits = 0;
    for (char c : cleaned) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) ++digits;
    }
    return digits * 2 >= cleaned.size();
}

// --------------------------------------------------------------- html ------

struct TableCandidate {
    std::vector<std::string> header;
    std::vector<const html::Node*> data_rows;
    const html::Node* table = nullptr;
    std::size_t score = 0;
};

std::vector<const html::Node*> row_cells(const html::Node* row) {
    std::vector<const html::Node*> cells;
    for (const html::NodePtr& child : row->children) {
        if (child->kind == html::Node::Kind::Element &&
            (child->tag == "td" || child->tag == "th")) {
            cells.push_back(child.get());
        }
    }
    return cells;
}

TableCandidate analyze_table(const html::Node* table) {
    TableCandidate out;
    out.table = table;

    std::vector<const html::Node*> rows;
    html::walk(table, [&](const html::Node* n) {
        if (n->kind == html::Node::Kind::Element && n->tag == "tr") rows.push_back(n);
    });
    if (rows.empty()) return out;

    // Header: first row made of th cells, else a first row that does not read
    // as data (no numeric-heavy cells).
    std::size_t first_data = 0;
    const std::vector<const html::Node*> first_cells = row_cells(rows[0]);
    bool header_found = false;
    if (!first_cells.empty()) {
        const bool all_th = std::all_of(first_cells.begin(), first_cells.end(),
                                        [](const html::Node* c) { return c->tag == "th"; });
        const bool no_numbers =
            std::none_of(first_cells.begin(), first_cells.end(), [](const html::Node* c) {
                return cell_text_is_numericish(c->text_content());
            });
        if (all_th || no_numbers) {
            for (const html::Node* cell : first_cells) out.header.push_back(cell->text_content());
            first_data = 1;
            header_found = true;
        }
    }
    for (std::size_t i = first_data; i < rows.size(); ++i) {
        const std::vector<const html::Node*> cells = row_cells(rows[i]);
        if (cells.size() >= 2) out.data_rows.push_back(rows[i]);
    }
    if (!header_found && !out.data_rows.empty()) {
        const std::size_t width = row_cells(out.data_rows[0]).size();
        for (std::size_t i = 0; i < width; ++i) out.header.push_back("col_" + std::to_string(i + 1));
    }
    out.score = out.data_rows.size() * out.header.size();
    return out;
}

const std::string* data_attr(const html::Node& node) {
    static const std::vector<std::string> kAttrs = {"data-field", "data-name", "data-key",
                                                    "data-label", "itemprop"};
    for (const std::string& a : kAttrs) {
        const std::string* v = node.attr(a);
        if (v != nullptr && !v->empty()) return v;
    }
    return nullptr;
}

// Extracts labelled cells from one repeated block. Labels come from, in order
// of preference: data attributes, a label child ("<span>Owner:</span> value"),
// an inline "Label: value" split, dt/dd pairing, then semantic class names.
void block_cells(const html::Node* block, const std::string& block_path, RawRecord& record) {
    std::string pending_dt;
    html::walk(block, [&](const html::Node* n) {
        if (n->kind != html::Node::Kind::Element) return;
        const std::string path = block_path + "/" + n->signature();

        if (n->tag == "dt") {
            pending_dt = n->text_content();
            return;
        }
        if (n->tag == "dd") {
            if (!pending_dt.empty()) {
                record.cells.push_back(Cell{pending_dt, n->text_content(), path});
                pending_dt.clear();
            }
            return;
        }

        const std::string* attr_label = data_attr(*n);
        if (attr_label != nullptr) {
            const std::string value = n->text_content();
            if (!value.empty()) record.cells.push_back(Cell{*attr_label, value, path});
            return;
        }

        // "<b>Owner:</b> Jane Smith" inside one element.
        const std::string own = n->own_text();
        if (!own.empty() && n->element_child_count() == 1) {
            const html::Node* child = nullptr;
            for (const html::NodePtr& c : n->children) {
                if (c->kind == html::Node::Kind::Element) child = c.get();
            }
            if (child != nullptr) {
                std::string label = child->text_content();
                if (!label.empty() && label.back() == ':') label.pop_back();
                label = str::trim(label);
                if (!label.empty() && label.size() <= 40) {
                    record.cells.push_back(Cell{label, own, path});
                    return;
                }
            }
        }

        // Leaf with "Label: value" in its own text.
        if (n->element_child_count() == 0 && !own.empty()) {
            const std::size_t colon = own.find(':');
            if (colon != std::string::npos && colon > 0 && colon <= 40 &&
                colon + 1 < own.size()) {
                const std::string label = str::trim(own.substr(0, colon));
                const std::string value = str::trim(own.substr(colon + 1));
                if (!label.empty() && !value.empty() && !cell_text_is_numericish(label)) {
                    record.cells.push_back(Cell{label, value, path});
                    return;
                }
            }
            const std::string label = class_label(*n);
            if (!label.empty()) {
                record.cells.push_back(Cell{label, own, path});
            }
        }
    });
}

struct BlockCandidate {
    const html::Node* parent = nullptr;
    std::string signature;
    std::vector<const html::Node*> blocks;
    std::size_t score = 0;
};

BlockCandidate best_repeated_block(const html::Node* root) {
    BlockCandidate best;
    html::walk(root, [&](const html::Node* parent) {
        if (parent->kind != html::Node::Kind::Element &&
            parent->kind != html::Node::Kind::Document) {
            return;
        }
        std::map<std::string, std::vector<const html::Node*>> groups;
        for (const html::NodePtr& child : parent->children) {
            if (child->kind != html::Node::Kind::Element) continue;
            if (child->tag == "tr" || child->tag == "td" || child->tag == "th") continue;
            groups[child->signature()].push_back(child.get());
        }
        for (const auto& [signature, blocks] : groups) {
            if (blocks.size() < 3) continue;
            RawRecord probe;
            block_cells(blocks[0], signature, probe);
            if (probe.cells.size() < 2) continue;
            const std::size_t score = blocks.size() * probe.cells.size();
            if (score > best.score) {
                best = BlockCandidate{parent, signature, blocks, score};
            }
        }
    });
    return best;
}

void extract_html(Model& model, std::string_view body) {
    const html::Document document = html::parse(body);
    model.title = document.title();
    model.headings = document.headings();
    model.text = document.text();

    TableCandidate best_table;
    for (const html::Node* table : document.find_all("table")) {
        TableCandidate candidate = analyze_table(table);
        if (candidate.score > best_table.score) best_table = std::move(candidate);
    }
    const BlockCandidate blocks = best_repeated_block(document.root());

    if (best_table.score >= blocks.score && best_table.score > 0) {
        model.container_signature = "table/" + best_table.table->signature();
        for (std::size_t r = 0; r < best_table.data_rows.size(); ++r) {
            const std::vector<const html::Node*> cells = row_cells(best_table.data_rows[r]);
            RawRecord record;
            for (std::size_t c = 0; c < cells.size(); ++c) {
                const std::string label = c < best_table.header.size()
                                              ? best_table.header[c]
                                              : "col_" + std::to_string(c + 1);
                const std::string value = cells[c]->text_content();
                if (value.empty()) continue;
                const std::string path = model.container_signature + "/tr[" + std::to_string(r) +
                                         "]/" + cells[c]->signature() + "[" + std::to_string(c) +
                                         "]";
                record.cells.push_back(Cell{label, value, path});
            }
            if (!record.cells.empty()) model.records.push_back(std::move(record));
        }
        return;
    }

    if (blocks.score > 0) {
        model.container_signature =
            (blocks.parent != nullptr ? blocks.parent->signature() : std::string{"#document"}) +
            "/" + blocks.signature;
        for (const html::Node* block : blocks.blocks) {
            RawRecord record;
            block_cells(block, model.container_signature, record);
            if (record.cells.size() >= 2) model.records.push_back(std::move(record));
        }
    }
}

// --------------------------------------------------------------- json ------

void flatten_object(const json::Value& object, const std::string& prefix,
                    const std::string& path, RawRecord& record) {
    for (const auto& [key, value] : object.members()) {
        const std::string label = prefix.empty() ? key : prefix + "." + key;
        const std::string here = path + "/" + key;
        if (value.is_object()) {
            flatten_object(value, label, here, record);
            continue;
        }
        if (value.is_array()) {
            std::vector<std::string> parts;
            for (const json::Value& item : value.items()) {
                if (item.is_scalar()) parts.push_back(item.to_display());
            }
            if (!parts.empty()) {
                record.cells.push_back(Cell{label, str::join(parts, "; "), here});
            }
            continue;
        }
        if (value.is_null()) continue;
        record.cells.push_back(Cell{label, value.to_display(), here});
    }
}

struct JsonArrayCandidate {
    const json::Value* array = nullptr;
    std::string path;
    std::size_t score = 0;
};

void find_record_arrays(const json::Value& value, const std::string& path,
                        JsonArrayCandidate& best) {
    if (value.is_array()) {
        std::size_t objects = 0;
        std::size_t keys = 0;
        for (const json::Value& item : value.items()) {
            if (item.is_object()) {
                ++objects;
                keys += item.members().size();
            }
        }
        if (objects >= 1 && objects == value.items().size()) {
            const std::size_t score = objects * (keys / std::max<std::size_t>(objects, 1));
            if (score > best.score) best = JsonArrayCandidate{&value, path, score};
        }
        for (std::size_t i = 0; i < value.items().size(); ++i) {
            find_record_arrays(value.items()[i], path + "/" + std::to_string(i), best);
        }
        return;
    }
    if (value.is_object()) {
        for (const auto& [key, member] : value.members()) {
            find_record_arrays(member, path + "/" + key, best);
        }
    }
}

void extract_json(Model& model, std::string_view body) {
    const json::Value root = json::parse(body);

    // Text for classification: keys and scalar values.
    std::string text;
    const std::function<void(const json::Value&)> collect = [&](const json::Value& v) {
        if (v.is_object()) {
            for (const auto& [key, member] : v.members()) {
                text += key;
                text.push_back(' ');
                collect(member);
            }
        } else if (v.is_array()) {
            for (const json::Value& item : v.items()) collect(item);
        } else if (v.is_string()) {
            text += v.as_string();
            text.push_back(' ');
        }
    };
    collect(root);
    model.text = str::collapse_ws(text);

    JsonArrayCandidate best;
    find_record_arrays(root, "", best);
    if (best.array == nullptr) {
        // A single object of scalars is one record (detail pages).
        if (root.is_object()) {
            RawRecord record;
            flatten_object(root, "", "", record);
            if (record.cells.size() >= 2) {
                model.records.push_back(std::move(record));
                model.container_signature = "object";
            }
        }
        return;
    }
    model.container_signature = "array" + (best.path.empty() ? std::string{"/"} : best.path);
    for (std::size_t i = 0; i < best.array->items().size(); ++i) {
        const json::Value& item = best.array->items()[i];
        RawRecord record;
        flatten_object(item, "", best.path + "/" + std::to_string(i), record);
        if (!record.cells.empty()) model.records.push_back(std::move(record));
    }
}

// ---------------------------------------------------------------- csv ------

void extract_csv(Model& model, std::string_view body) {
    const csv::Table table = csv::parse(body);
    std::vector<std::string> header = table.header;
    if (header.empty() && !table.rows.empty()) {
        for (std::size_t i = 0; i < table.rows[0].size(); ++i) {
            header.push_back("col_" + std::to_string(i + 1));
        }
    }
    model.container_signature = "csv[" + std::to_string(header.size()) + "]";
    model.text = std::string{body.substr(0, std::min<std::size_t>(body.size(), 4000))};
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        RawRecord record;
        for (std::size_t c = 0; c < table.rows[r].size() && c < header.size(); ++c) {
            const std::string value = str::trim(table.rows[r][c]);
            if (value.empty()) continue;
            record.cells.push_back(
                Cell{header[c], value, "row[" + std::to_string(r) + "]/col[" + std::to_string(c) + "]"});
        }
        if (!record.cells.empty()) model.records.push_back(std::move(record));
    }
}

// ----------------------------------------------------- pdf and plain text --

std::vector<std::string> split_columns(const std::string& line) {
    std::vector<std::string> cols;
    std::string current;
    std::size_t spaces = 0;
    for (char c : line) {
        if (c == '\t') {
            spaces = 2;
            continue;
        }
        if (c == ' ') {
            ++spaces;
            continue;
        }
        if (spaces >= 2 && !current.empty()) {
            cols.push_back(str::trim(current));
            current.clear();
        } else if (spaces > 0 && !current.empty()) {
            current.push_back(' ');
        }
        spaces = 0;
        current.push_back(c);
    }
    if (!str::trim(current).empty()) cols.push_back(str::trim(current));
    return cols;
}

void extract_lines(Model& model, const std::vector<std::string>& lines) {
    model.text = str::collapse_ws(str::join(lines, " "));
    if (!lines.empty()) model.title = lines.front();

    // Dominant column count across whitespace-aligned lines.
    std::map<std::size_t, std::size_t> width_votes;
    std::vector<std::vector<std::string>> split;
    split.reserve(lines.size());
    for (const std::string& line : lines) {
        split.push_back(split_columns(line));
        if (split.back().size() >= 2) ++width_votes[split.back().size()];
    }
    std::size_t width = 0;
    std::size_t votes = 0;
    for (const auto& [w, v] : width_votes) {
        if (v > votes) {
            votes = v;
            width = w;
        }
    }
    if (width < 2 || votes < 3) return;

    std::vector<std::string> header;
    for (std::size_t i = 0; i < split.size(); ++i) {
        if (split[i].size() != width) continue;
        if (header.empty()) {
            const bool numeric = std::any_of(split[i].begin(), split[i].end(),
                                             [](const std::string& c) {
                                                 return cell_text_is_numericish(c);
                                             });
            if (!numeric) {
                header = split[i];
                continue;
            }
            for (std::size_t c = 0; c < width; ++c) header.push_back("col_" + std::to_string(c + 1));
        }
        RawRecord record;
        for (std::size_t c = 0; c < width; ++c) {
            if (split[i][c].empty()) continue;
            record.cells.push_back(Cell{header[c], split[i][c],
                                        "line[" + std::to_string(i) + "]/col[" + std::to_string(c) + "]"});
        }
        if (!record.cells.empty()) model.records.push_back(std::move(record));
    }
    model.container_signature = "text[" + std::to_string(width) + "]";
}

} // namespace

std::string_view format_name(Format f) {
    switch (f) {
    case Format::Html: return "html";
    case Format::Json: return "json";
    case Format::Csv: return "csv";
    case Format::Pdf: return "pdf";
    case Format::Text: return "text";
    case Format::Unknown: return "unknown";
    }
    return "unknown";
}

Format detect_format(std::string_view content_type, std::string_view body) {
    if (pdf::looks_like_pdf(body)) return Format::Pdf;

    const std::string trimmed = str::trim(body.substr(0, 512));
    if (!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[')) {
        try {
            json::parse(body);
            return Format::Json;
        } catch (const Error&) {
            // fall through: JSON-looking but malformed
        }
    }
    const std::string lowered = str::to_lower(trimmed.substr(0, 256));
    if (str::contains(lowered, "<html") || str::contains(lowered, "<!doctype") ||
        str::contains(lowered, "<table") || str::contains(lowered, "<div") ||
        str::contains(lowered, "<body") || str::contains(lowered, "<head")) {
        return Format::Html;
    }

    const std::string type = str::to_lower(content_type);
    if (str::contains(type, "html")) return Format::Html;
    if (str::contains(type, "json")) return Format::Json;
    if (str::contains(type, "csv")) return Format::Csv;
    if (str::contains(type, "pdf")) return Format::Pdf;

    // CSV shape: consistent delimiter counts over the first lines.
    const char delim = csv::detect_delimiter(body);
    std::size_t consistent = 0;
    std::size_t expected = std::string::npos;
    std::size_t line_start = 0;
    for (std::size_t checked = 0; checked < 6 && line_start < body.size(); ++checked) {
        std::size_t eol = body.find('\n', line_start);
        if (eol == std::string_view::npos) eol = body.size();
        const std::string_view line = body.substr(line_start, eol - line_start);
        const std::size_t count =
            static_cast<std::size_t>(std::count(line.begin(), line.end(), delim));
        if (expected == std::string::npos) expected = count;
        if (count == expected && count >= 1) ++consistent;
        line_start = eol + 1;
    }
    if (consistent >= 2) return Format::Csv;
    if (str::contains(lowered, "<")) return Format::Html;
    if (trimmed.empty()) return Format::Unknown;
    return Format::Text;
}

const Cell* RawRecord::find(std::string_view label) const {
    for (const Cell& cell : cells) {
        if (cell.label == label) return &cell;
    }
    return nullptr;
}

std::string Model::structure_fingerprint() const {
    std::vector<std::string> sorted = labels;
    std::sort(sorted.begin(), sorted.end());
    std::string basis{format_name(format)};
    basis.push_back('|');
    basis += container_signature;
    basis.push_back('|');
    basis += str::join(sorted, ",");
    return str::hex64(str::hash64(basis));
}

Model build(Format format, std::string_view body) {
    Model model;
    model.format = format;
    switch (format) {
    case Format::Html: extract_html(model, body); break;
    case Format::Json: extract_json(model, body); break;
    case Format::Csv: extract_csv(model, body); break;
    case Format::Pdf: extract_lines(model, pdf::extract_text_lines(body)); break;
    case Format::Text: {
        std::vector<std::string> lines;
        for (const std::string& line : str::split(std::string{body}, '\n')) {
            if (!str::trim(line).empty()) lines.push_back(line);
        }
        extract_lines(model, lines);
        break;
    }
    case Format::Unknown: break;
    }
    finish(model);
    return model;
}

Model build_auto(std::string_view content_type, std::string_view body) {
    return build(detect_format(content_type, body), body);
}

} // namespace dd::doc
