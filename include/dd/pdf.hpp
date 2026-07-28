#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dd::pdf {

bool looks_like_pdf(std::string_view bytes);

// Text lines extracted from the page content streams, in document order.
// Column gaps that the PDF encodes as large kerning adjustments come out as
// multi-space runs so a table sniffer can split them again.
//
// This reader handles the common county-report shape: Flate or plain content
// streams with BT/ET text blocks, Tj/TJ/' show operators, and Td/TD/T* line
// moves. It throws dd::Error when the bytes are not a PDF; a PDF from which no
// text can be recovered yields an empty vector, which the pipeline reports as
// an extraction failure rather than inventing content.
std::vector<std::string> extract_text_lines(std::string_view bytes);

} // namespace dd::pdf
