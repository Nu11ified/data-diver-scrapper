#pragma once

#include "dd/engine/schema.hpp"
#include "dd/parse/document.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// The validity benchmark: a hand-verified answer key (data/golden) scored
// against whatever produced a classification and a mapping - this engine or
// an LLM baseline. Scoring is shared so both sit in the same table.
namespace dd::bench {

// One source's answer key. `classifications` lists every acceptable class.
// `fields` maps a canonical field to its acceptable source labels; an empty
// string in the list means leaving the field unmapped is also acceptable; a
// field absent from the map must stay unmapped.
struct Golden {
    std::string source_id;
    std::vector<std::string> classifications;
    std::map<std::string, std::vector<std::string>> fields;
};

std::vector<Golden> load_golden(const std::string& path);

struct MappingScore {
    std::size_t tp = 0;        // mapped to an acceptable label
    std::size_t spurious = 0;  // mapped to a label the key rejects
    std::size_t missing = 0;   // unmapped where the key required a label

    double precision() const noexcept;
    double recall() const noexcept;
    double f1() const noexcept;
};

bool classification_ok(const Golden& golden, const std::string& predicted);

MappingScore score_mapping(const Golden& golden, const schema::Registry& registry,
                           const std::map<std::string, std::string>& mapped);

// The baseline's task, stated once: same document labels with sample values,
// same schema, same classes; strict-JSON answer.
std::string llm_prompt(const schema::Registry& registry,
                       const std::vector<std::string>& classes, const doc::Model& model);

struct LlmAnswer {
    bool ok = false;
    std::string classification;
    std::map<std::string, std::string> mapping;
    std::string error;
};

// Tolerates a markdown code fence around the JSON; anything else non-JSON
// is an error that shows up in the run, never a guessed answer.
LlmAnswer parse_llm_answer(std::string_view text);

} // namespace dd::bench
