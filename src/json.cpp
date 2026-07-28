#include "dd/json.hpp"

#include "dd/core.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dd::json {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : text_{text} {}

    Value parse_document() {
        skip_ws();
        Value v = parse_value(0);
        skip_ws();
        if (pos_ != text_.size()) fail("trailing content after JSON value");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& why) const {
        throw Error("json: " + why + " at offset " + std::to_string(pos_));
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
                continue;
            }
            break;
        }
    }

    char peek() const {
        if (pos_ >= text_.size()) fail("unexpected end of input");
        return text_[pos_];
    }

    bool literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0) return false;
        pos_ += word.size();
        return true;
    }

    Value parse_value(int depth) {
        if (depth > kMaxDepth) fail("nesting too deep");
        switch (peek()) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': return Value::string(parse_string());
        case 't':
            if (!literal("true")) fail("bad literal");
            return Value::boolean(true);
        case 'f':
            if (!literal("false")) fail("bad literal");
            return Value::boolean(false);
        case 'n':
            if (!literal("null")) fail("bad literal");
            return Value{};
        default: return Value::number(parse_number());
        }
    }

    Value parse_object(int depth) {
        ++pos_; // '{'
        Members members;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return Value::object(std::move(members));
        }
        while (true) {
            skip_ws();
            if (peek() != '"') fail("expected object key");
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') fail("expected ':'");
            ++pos_;
            skip_ws();
            members.emplace_back(std::move(key), parse_value(depth + 1));
            skip_ws();
            const char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                return Value::object(std::move(members));
            }
            fail("expected ',' or '}'");
        }
    }

    Value parse_array(int depth) {
        ++pos_; // '['
        std::vector<Value> items;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return Value::array(std::move(items));
        }
        while (true) {
            skip_ws();
            items.push_back(parse_value(depth + 1));
            skip_ws();
            const char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == ']') {
                ++pos_;
                return Value::array(std::move(items));
            }
            fail("expected ',' or ']'");
        }
    }

    void append_utf8(std::string& out, unsigned int code) const {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    unsigned int parse_hex4() {
        if (pos_ + 4 > text_.size()) fail("truncated \\u escape");
        unsigned int value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
            else fail("bad hex digit in \\u escape");
        }
        return value;
    }

    std::string parse_string() {
        ++pos_; // opening quote
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) fail("unterminated escape");
            const char esc = text_[pos_++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned int code = parse_hex4();
                if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                    text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                    pos_ += 2;
                    const unsigned int low = parse_hex4();
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        append_utf8(out, code);
                        code = low;
                    }
                }
                append_utf8(out, code);
                break;
            }
            default: fail("unknown escape");
            }
        }
    }

    double parse_number() {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool any_digit = false;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
            any_digit = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
                any_digit = true;
            }
        }
        if (!any_digit) fail("expected a number");
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }
        const std::string chunk{text_.substr(start, pos_ - start)};
        return std::strtod(chunk.c_str(), nullptr);
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

} // namespace

Value Value::boolean(bool v) {
    Value out;
    out.type_ = Type::Bool;
    out.bool_ = v;
    return out;
}

Value Value::number(double v) {
    Value out;
    out.type_ = Type::Number;
    out.number_ = v;
    return out;
}

Value Value::string(std::string v) {
    Value out;
    out.type_ = Type::String;
    out.string_ = std::move(v);
    return out;
}

Value Value::array(std::vector<Value> v) {
    Value out;
    out.type_ = Type::Array;
    out.array_ = std::move(v);
    return out;
}

Value Value::object(Members v) {
    Value out;
    out.type_ = Type::Object;
    out.object_ = std::move(v);
    return out;
}

bool Value::as_bool(bool fallback) const noexcept {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return number_ != 0.0;
    return fallback;
}

double Value::as_number(double fallback) const noexcept {
    if (type_ == Type::Number) return number_;
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    if (type_ == Type::String) {
        try {
            return std::stod(string_);
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

std::string Value::as_string() const {
    if (type_ == Type::String) return string_;
    return to_display();
}

const Value* Value::find(std::string_view key) const noexcept {
    for (const auto& [name, value] : object_) {
        if (name == key) return &value;
    }
    return nullptr;
}

std::string Value::to_display() const {
    switch (type_) {
    case Type::Null: return {};
    case Type::Bool: return bool_ ? "true" : "false";
    case Type::Number: return format_number(number_);
    case Type::String: return string_;
    case Type::Array:
    case Type::Object: return serialize();
    }
    return {};
}

std::string Value::serialize() const {
    switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return bool_ ? "true" : "false";
    case Type::Number: return format_number(number_);
    case Type::String: return quote(string_);
    case Type::Array: {
        std::string out = "[";
        for (std::size_t i = 0; i < array_.size(); ++i) {
            if (i != 0) out.push_back(',');
            out += array_[i].serialize();
        }
        out.push_back(']');
        return out;
    }
    case Type::Object: {
        std::string out = "{";
        for (std::size_t i = 0; i < object_.size(); ++i) {
            if (i != 0) out.push_back(',');
            out += quote(object_[i].first);
            out.push_back(':');
            out += object_[i].second.serialize();
        }
        out.push_back('}');
        return out;
    }
    }
    return "null";
}

Value parse(std::string_view text) {
    Parser parser{text};
    return parser.parse_document();
}

std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (uc < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", uc);
                out += buffer;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
    return out;
}

std::string format_number(double v) {
    if (!std::isfinite(v)) return "0";
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(v));
        return std::string{buffer};
    }
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.10g", v);
    return std::string{buffer};
}

void Writer::prefix() {
    if (pending_value_) {
        pending_value_ = false;
        return;
    }
    if (first_.empty()) return;
    if (!first_.back()) out_.push_back(',');
    first_.back() = false;
}

void Writer::begin_object() {
    prefix();
    out_.push_back('{');
    first_.push_back(true);
}

void Writer::end_object() {
    out_.push_back('}');
    if (!first_.empty()) first_.pop_back();
}

void Writer::begin_array() {
    prefix();
    out_.push_back('[');
    first_.push_back(true);
}

void Writer::end_array() {
    out_.push_back(']');
    if (!first_.empty()) first_.pop_back();
}

void Writer::key(std::string_view k) {
    prefix();
    out_ += quote(k);
    out_.push_back(':');
    pending_value_ = true;
}

void Writer::string_value(std::string_view v) {
    prefix();
    out_ += quote(v);
}

void Writer::number_value(double v) {
    prefix();
    out_ += format_number(v);
}

void Writer::integer_value(std::int64_t v) {
    prefix();
    out_ += std::to_string(v);
}

void Writer::bool_value(bool v) {
    prefix();
    out_ += v ? "true" : "false";
}

void Writer::null_value() {
    prefix();
    out_ += "null";
}

void Writer::raw_value(std::string_view already_json) {
    prefix();
    out_ += already_json;
}

void Writer::field(std::string_view k, std::string_view v) {
    key(k);
    string_value(v);
}

void Writer::field(std::string_view k, const char* v) { field(k, std::string_view{v}); }

void Writer::field(std::string_view k, double v) {
    key(k);
    number_value(v);
}

void Writer::field(std::string_view k, std::int64_t v) {
    key(k);
    integer_value(v);
}

void Writer::field(std::string_view k, int v) {
    key(k);
    integer_value(v);
}

void Writer::field(std::string_view k, bool v) {
    key(k);
    bool_value(v);
}

void Writer::field_raw(std::string_view k, std::string_view already_json) {
    key(k);
    raw_value(already_json);
}

} // namespace dd::json
