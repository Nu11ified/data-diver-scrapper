#include "dd/parse/pdf.hpp"

#include "dd/core/core.hpp"

#include <cctype>
#include <cmath>
#include <cstring>

#if defined(DD_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace dd::pdf {
namespace {

// A PDF content stream token: string data to show, a number, or an operator.
struct Token {
    enum class Kind { String, Number, Operator, ArrayOpen, ArrayClose, Name };
    Kind kind;
    std::string text;
    double number = 0.0;
};

#if defined(DD_HAVE_ZLIB)
std::string inflate_stream(std::string_view compressed) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return {};
    std::string out;
    char buffer[16384];
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    zs.avail_in = static_cast<uInt>(compressed.size());
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof(buffer);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_OK || rc == Z_STREAM_END) {
            out.append(buffer, sizeof(buffer) - zs.avail_out);
        }
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return {};
    return out;
}
#endif

// Splits the raw bytes into decoded stream contents. Works without the xref
// table: county PDFs frequently have broken cross references, and for text
// recovery a linear object scan is enough.
std::vector<std::string> decoded_streams(std::string_view bytes) {
    std::vector<std::string> streams;
    std::size_t pos = 0;
    while (true) {
        const std::size_t stream_kw = bytes.find("stream", pos);
        if (stream_kw == std::string_view::npos) break;

        // The dictionary for this stream sits between the previous "obj" (or
        // start) and the keyword.
        const std::size_t obj_kw = bytes.rfind("obj", stream_kw);
        const std::size_t dict_begin = obj_kw == std::string_view::npos ? 0 : obj_kw;
        const std::string_view dict = bytes.substr(dict_begin, stream_kw - dict_begin);

        std::size_t data_begin = stream_kw + 6;
        if (data_begin < bytes.size() && bytes[data_begin] == '\r') ++data_begin;
        if (data_begin < bytes.size() && bytes[data_begin] == '\n') ++data_begin;

        const std::size_t data_end = bytes.find("endstream", data_begin);
        if (data_end == std::string_view::npos) break;
        std::size_t trimmed_end = data_end;
        while (trimmed_end > data_begin &&
               (bytes[trimmed_end - 1] == '\n' || bytes[trimmed_end - 1] == '\r')) {
            --trimmed_end;
        }
        const std::string_view data = bytes.substr(data_begin, trimmed_end - data_begin);
        pos = data_end + 9;

        const bool is_image = dd::str::contains(dict, "/Image");
        if (is_image) continue;

        if (dd::str::contains(dict, "/FlateDecode")) {
#if defined(DD_HAVE_ZLIB)
            std::string inflated = inflate_stream(data);
            if (!inflated.empty()) streams.push_back(std::move(inflated));
#endif
            continue;
        }
        // Other filters (DCT, LZW, ASCII85) do not carry the text we need or
        // are rare in generated reports; skipping them loses nothing we could
        // honestly recover here.
        if (dd::str::contains(dict, "/Filter")) continue;
        streams.emplace_back(data);
    }
    return streams;
}

class Tokenizer {
public:
    explicit Tokenizer(std::string_view text) : text_{text} {}

    bool next(Token& out) {
        skip_ws();
        if (pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '%') { // comment to end of line
            while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
            return next(out);
        }
        if (c == '(') return read_literal_string(out);
        if (c == '<') {
            if (pos_ + 1 < text_.size() && text_[pos_ + 1] == '<') {
                pos_ += 2;
                out = Token{Token::Kind::Operator, "<<", 0.0};
                return true;
            }
            return read_hex_string(out);
        }
        if (c == '>') {
            if (pos_ + 1 < text_.size() && text_[pos_ + 1] == '>') pos_ += 2;
            else ++pos_;
            out = Token{Token::Kind::Operator, ">>", 0.0};
            return true;
        }
        if (c == '[') {
            ++pos_;
            out = Token{Token::Kind::ArrayOpen, "[", 0.0};
            return true;
        }
        if (c == ']') {
            ++pos_;
            out = Token{Token::Kind::ArrayClose, "]", 0.0};
            return true;
        }
        if (c == '/') return read_name(out);
        if (c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
            return read_number(out);
        }
        return read_operator(out);
    }

private:
    void skip_ws() {
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (std::isspace(c) != 0 || c == '\0') ++pos_;
            else break;
        }
    }

    bool read_literal_string(Token& out) {
        ++pos_; // '('
        std::string value;
        int depth = 1;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '\\') {
                if (pos_ >= text_.size()) break;
                const char esc = text_[pos_++];
                switch (esc) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case '(': value.push_back('('); break;
                case ')': value.push_back(')'); break;
                case '\\': value.push_back('\\'); break;
                case '\r':
                    if (pos_ < text_.size() && text_[pos_] == '\n') ++pos_;
                    break; // line continuation
                case '\n': break;
                default:
                    if (esc >= '0' && esc <= '7') {
                        int code = esc - '0';
                        for (int i = 0; i < 2 && pos_ < text_.size(); ++i) {
                            const char d = text_[pos_];
                            if (d < '0' || d > '7') break;
                            code = code * 8 + (d - '0');
                            ++pos_;
                        }
                        value.push_back(static_cast<char>(code));
                    } else {
                        value.push_back(esc);
                    }
                }
                continue;
            }
            if (c == '(') {
                ++depth;
                value.push_back(c);
                continue;
            }
            if (c == ')') {
                --depth;
                if (depth == 0) break;
                value.push_back(c);
                continue;
            }
            value.push_back(c);
        }
        out = Token{Token::Kind::String, std::move(value), 0.0};
        return true;
    }

    bool read_hex_string(Token& out) {
        ++pos_; // '<'
        std::string value;
        int nibble = -1;
        while (pos_ < text_.size() && text_[pos_] != '>') {
            const char c = text_[pos_++];
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else continue;
            if (nibble < 0) {
                nibble = v;
            } else {
                value.push_back(static_cast<char>((nibble << 4) | v));
                nibble = -1;
            }
        }
        if (nibble >= 0) value.push_back(static_cast<char>(nibble << 4));
        if (pos_ < text_.size()) ++pos_; // '>'
        out = Token{Token::Kind::String, std::move(value), 0.0};
        return true;
    }

    bool read_name(Token& out) {
        ++pos_; // '/'
        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (std::isspace(c) != 0 || std::strchr("()<>[]{}/%", c) != nullptr) break;
            ++pos_;
        }
        out = Token{Token::Kind::Name, std::string{text_.substr(start, pos_ - start)}, 0.0};
        return true;
    }

    bool read_number(Token& out) {
        const std::size_t start = pos_;
        if (text_[pos_] == '-' || text_[pos_] == '+') ++pos_;
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0 || text_[pos_] == '.')) {
            ++pos_;
        }
        const std::string chunk{text_.substr(start, pos_ - start)};
        out = Token{Token::Kind::Number, chunk, std::atof(chunk.c_str())};
        return true;
    }

    bool read_operator(Token& out) {
        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (std::isspace(c) != 0 || std::strchr("()<>[]{}/%", c) != nullptr) break;
            ++pos_;
        }
        if (pos_ == start) ++pos_; // always progress
        out = Token{Token::Kind::Operator, std::string{text_.substr(start, pos_ - start)}, 0.0};
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

// Kern adjustments beyond this many thousandths of an em read as a column
// gap rather than letter spacing.
constexpr double kColumnKern = 180.0;

void flush_line(std::vector<std::string>& lines, std::string& line) {
    const std::string trimmed = [&] {
        std::string t = line;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        return t;
    }();
    if (!dd::str::trim(trimmed).empty()) lines.push_back(trimmed);
    line.clear();
}

void extract_from_stream(std::string_view content, std::vector<std::string>& lines) {
    Tokenizer tokenizer{content};
    Token token;
    std::vector<Token> stack;
    std::string line;
    double last_y = NAN;

    auto shown = [&](const std::string& s) {
        line += s;
        line.push_back(' ');
    };

    while (tokenizer.next(token)) {
        if (token.kind != Token::Kind::Operator) {
            stack.push_back(token);
            continue;
        }
        const std::string& op = token.text;
        if (op == "Tj") {
            if (!stack.empty() && stack.back().kind == Token::Kind::String) {
                shown(stack.back().text);
            }
        } else if (op == "TJ") {
            // Stack holds ArrayOpen, elements..., ArrayClose.
            std::string joined;
            for (const Token& t : stack) {
                if (t.kind == Token::Kind::String) {
                    joined += t.text;
                } else if (t.kind == Token::Kind::Number && t.number < -kColumnKern) {
                    joined += "  ";
                }
            }
            if (!joined.empty()) shown(joined);
        } else if (op == "'" || op == "\"") {
            flush_line(lines, line);
            if (!stack.empty() && stack.back().kind == Token::Kind::String) {
                shown(stack.back().text);
            }
        } else if (op == "Td" || op == "TD") {
            if (stack.size() >= 2 && stack[stack.size() - 1].kind == Token::Kind::Number) {
                const double ty = stack[stack.size() - 1].number;
                if (ty != 0.0) flush_line(lines, line);
                else line += "  "; // horizontal move on the same baseline
            } else {
                flush_line(lines, line);
            }
        } else if (op == "T*") {
            flush_line(lines, line);
        } else if (op == "Tm") {
            if (stack.size() >= 6 && stack.back().kind == Token::Kind::Number) {
                const double y = stack.back().number;
                if (std::isnan(last_y) || std::fabs(y - last_y) > 0.5) {
                    flush_line(lines, line);
                } else {
                    line += "  ";
                }
                last_y = y;
            } else {
                flush_line(lines, line);
            }
        } else if (op == "ET" || op == "BT") {
            flush_line(lines, line);
        }
        stack.clear();
    }
    flush_line(lines, line);
}

} // namespace

bool looks_like_pdf(std::string_view bytes) {
    return bytes.size() > 8 && bytes.substr(0, 5) == "%PDF-";
}

std::vector<std::string> extract_text_lines(std::string_view bytes) {
    if (!looks_like_pdf(bytes)) throw Error("not a PDF: missing %PDF- header");
    std::vector<std::string> lines;
    for (const std::string& stream : decoded_streams(bytes)) {
        if (!str::contains(stream, "BT")) continue;
        extract_from_stream(stream, lines);
    }
    return lines;
}

} // namespace dd::pdf
