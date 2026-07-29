#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dd::json {
class Value;
using Members = std::vector<std::pair<std::string, Value>>;

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    static Value boolean(bool v);
    static Value number(double v);
    static Value string(std::string v);
    static Value array(std::vector<Value> v);
    static Value object(Members v);

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_number() const noexcept { return type_ == Type::Number; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }
    bool is_scalar() const noexcept { return !is_array() && !is_object(); }

    bool as_bool(bool fallback = false) const noexcept;
    double as_number(double fallback = 0.0) const noexcept;
    std::string as_string() const;

    const std::vector<Value>& items() const noexcept { return array_; }
    const Members& members() const noexcept { return object_; }
    const Value* find(std::string_view key) const noexcept;

    std::string to_display() const;
    std::string serialize() const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> array_;
    Members object_;
};

Value parse(std::string_view text);

std::string quote(std::string_view s);
std::string format_number(double v);

class Writer {
public:
    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    void key(std::string_view k);
    void string_value(std::string_view v);
    void number_value(double v);
    void integer_value(std::int64_t v);
    void bool_value(bool v);
    void null_value();
    void raw_value(std::string_view already_json);

    void field(std::string_view k, std::string_view v);
    void field(std::string_view k, const char* v);
    void field(std::string_view k, double v);
    void field(std::string_view k, std::int64_t v);
    void field(std::string_view k, int v);
    void field(std::string_view k, bool v);
    void field_raw(std::string_view k, std::string_view already_json);

    const std::string& str() const noexcept { return out_; }
    std::string take() { return std::move(out_); }

private:
    void prefix();

    std::string out_;
    std::vector<bool> first_;
    bool pending_value_ = false;
};
} // namespace dd::json
