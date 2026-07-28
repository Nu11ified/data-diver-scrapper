#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Terminal presentation for the datadiver CLI. Pure output: nothing in here
// computes engine results, it only draws what was measured. Colors follow
// the terminal: disabled when stdout is not a tty or NO_COLOR is set.
namespace dd::render {

bool colors_enabled();

// Wraps `s` in an ANSI style when colors are on; returns `s` unchanged
// otherwise. Styles: "dim", "bold", "teal", "red", "amber", "green".
std::string paint(std::string_view style, std::string_view s);

// A one-line section heading: "== Classify ==================".
void section(std::string_view title);

// key: value line with a dim key, aligned to the widest key of the group.
void kv(const std::vector<std::pair<std::string, std::string>>& pairs);

// A horizontal meter for a [0,1] value: "#######---- 63%". Painted teal when
// high, amber when middling, red when low.
std::string meter(double value, int width = 12);

// Status word painted by tone: ok/healed -> teal, review -> amber,
// failed/drift -> red.
std::string stamp(std::string_view word);

// Monospace table with auto-sized columns; header drawn dim. Cells may carry
// ANSI styles; width accounting strips them.
void table(const std::vector<std::string>& header,
           const std::vector<std::vector<std::string>>& rows);

} // namespace dd::render
