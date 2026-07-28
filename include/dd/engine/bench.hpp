#pragma once

#include "dd/engine/schema.hpp"
#include "dd/parse/document.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// The validity benchmark: a hand-verified answer key (data/golden) scored
// against the engine's classification and mapping.
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

} // namespace dd::bench
