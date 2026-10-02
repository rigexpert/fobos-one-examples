#include "json.h"

#include <cctype>
#include <cmath>
#include <sstream>

namespace js {

bool Value::is_num() const {
    return type == Type::Num;
}

bool Value::is_bool() const {
    return type == Type::Bool;
}

bool Value::is_arr() const {
    return type == Type::Arr;
}

ValuePtr Value::get(const std::string& key) const {
    if (type != Type::Obj) {
        return nullptr;
    }
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : it->second;
}

bool Value::has(const std::string& key) const {
    return get(key) != nullptr;
}

double Value::num_or(const std::string& key, double fallback) const {
    ValuePtr v = get(key);
    return (v && v->is_num()) ? v->num : fallback;
}

bool Value::bool_or(const std::string& key, bool fallback) const {
    ValuePtr v = get(key);
    if (!v) {
        return fallback;
    }
    if (v->is_bool()) {
        return v->b;
    }
    if (v->is_num()) {
        return v->num != 0;
    }
    return fallback;
}

std::string Value::str_or(const std::string& key, const std::string& fallback) const {
    ValuePtr v = get(key);
    return (v && v->type == Type::Str) ? v->str : fallback;
}

namespace {

/// @brief Recursive-descent JSON parser, kept internal to this translation unit.
class Parser {
public:
    /// @brief Construct over the text to parse. @param s The JSON text (borrowed).
    explicit Parser(const std::string& s) : s_(s) {}

    /// @brief Parse the document. @return The root value, or nullptr on empty input.
    ValuePtr parse() {
        skip();
        return value();
    }

private:
    const std::string& s_;  ///< The text being parsed.
    size_t i_ = 0;          ///< Current read offset.

    /// @brief Advance past whitespace.
    void skip() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            i_++;
        }
    }

    /// @brief Consume the next char if it matches. @param c Expected char. @return true if consumed.
    bool eat(char c) {
        skip();
        if (i_ < s_.size() && s_[i_] == c) {
            i_++;
            return true;
        }
        return false;
    }

    /// @brief Parse any JSON value, dispatching on the next character. @return The value.
    ValuePtr value() {
        skip();
        if (i_ >= s_.size()) {
            return nullptr;
        }
        char c = s_[i_];
        if (c == '{') {
            return object();
        }
        if (c == '[') {
            return array();
        }
        if (c == '"') {
            return string_v();
        }
        if (c == 't' || c == 'f') {
            return boolean();
        }
        if (c == 'n') {
            i_ += 4;
            auto v = std::make_shared<Value>();
            v->type = Type::Null;
            return v;
        }
        return number();
    }

    /// @brief Parse an object (assumes the next char is '{'). @return The object value.
    ValuePtr object() {
        auto v = std::make_shared<Value>();
        v->type = Type::Obj;
        eat('{');
        skip();
        if (eat('}')) {
            return v;
        }
        while (true) {
            skip();
            ValuePtr key = string_v();
            if (!key) {
                break;
            }
            eat(':');
            v->obj[key->str] = value();
            skip();
            if (eat(',')) {
                continue;
            }
            eat('}');
            break;
        }
        return v;
    }

    /// @brief Parse an array (assumes the next char is '['). @return The array value.
    ValuePtr array() {
        auto v = std::make_shared<Value>();
        v->type = Type::Arr;
        eat('[');
        skip();
        if (eat(']')) {
            return v;
        }
        while (true) {
            v->arr.push_back(value());
            skip();
            if (eat(',')) {
                continue;
            }
            eat(']');
            break;
        }
        return v;
    }

    /// @brief Parse a string. @return The string value, or nullptr if not a string.
    ValuePtr string_v() {
        auto v = std::make_shared<Value>();
        v->type = Type::Str;
        skip();
        if (i_ >= s_.size() || s_[i_] != '"') {
            return nullptr;
        }
        i_++;
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c == '\\' && i_ < s_.size()) {
                char e = s_[i_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        i_++;  // closing quote
        v->str = out;
        return v;
    }

    /// @brief Parse a boolean literal (true/false). @return The boolean value.
    ValuePtr boolean() {
        auto v = std::make_shared<Value>();
        v->type = Type::Bool;
        if (s_.compare(i_, 4, "true") == 0) {
            v->b = true;
            i_ += 4;
        } else {
            v->b = false;
            i_ += 5;
        }
        return v;
    }

    /// @brief Parse a number literal. @return The numeric value (0 if unparseable).
    ValuePtr number() {
        size_t start = i_;
        while (i_ < s_.size() &&
               (isdigit((unsigned char)s_[i_]) || s_[i_] == '-' || s_[i_] == '+' ||
                s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) {
            i_++;
        }
        auto v = std::make_shared<Value>();
        v->type = Type::Num;
        try {
            v->num = std::stod(s_.substr(start, i_ - start));
        } catch (...) {
            v->num = 0;
        }
        return v;
    }
};

}  // namespace

ValuePtr parse(const std::string& text) {
    return Parser(text).parse();
}

std::string num_to_str(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        return "null";
    }
    std::ostringstream o;
    if (value == (long long)value && std::fabs(value) < 1e15) {
        o << (long long)value;
    } else {
        o.precision(10);
        o << value;
    }
    return o.str();
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += c; break;
        }
    }
    o += "\"";
    return o;
}

}  // namespace js
