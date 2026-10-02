/**
 * @file json.h
 * @brief Minimal JSON parser + builder for the spectrum server.
 *
 * Supports only what the REST bodies/responses need: null, bool, number, string,
 * array, and object. Parsing is deliberately lenient (it never throws).
 */
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace js {

struct Value;

/// Shared-ownership handle to a parsed JSON @ref Value.
using ValuePtr = std::shared_ptr<Value>;

/// JSON value kind.
enum class Type { Null, Bool, Num, Str, Arr, Obj };

/**
 * @brief A parsed JSON value.
 *
 * The active field depends on #type: #b for Bool, #num for Num, #str for Str, #arr for
 * Arr, and #obj for Obj. Object/array elements are held as ValuePtr children.
 */
struct Value {
    Type type = Type::Null;            ///< Which kind of value this is.
    bool b = false;                    ///< Value when @ref type is Bool.
    double num = 0;                    ///< Value when @ref type is Num.
    std::string str;                   ///< Value when @ref type is Str.
    std::vector<ValuePtr> arr;         ///< Elements when @ref type is Arr.
    std::map<std::string, ValuePtr> obj;  ///< Members when @ref type is Obj.

    /// @return true if this value is a number.
    bool is_num() const;
    /// @return true if this value is a boolean.
    bool is_bool() const;
    /// @return true if this value is an array.
    bool is_arr() const;

    /**
     * @brief Look up an object member.
     * @param key Member name.
     * @return The member value, or nullptr if this is not an object or the key is absent.
     */
    ValuePtr get(const std::string& key) const;

    /**
     * @brief Test whether an object member exists.
     * @param key Member name.
     * @return true if the member is present.
     */
    bool has(const std::string& key) const;

    /**
     * @brief Read a numeric member with a fallback.
     * @param key Member name.
     * @param fallback Returned if the member is missing or not a number.
     * @return The member's number, or @p fallback.
     */
    double num_or(const std::string& key, double fallback) const;

    /**
     * @brief Read a boolean member with a fallback (numbers are treated as truthy).
     * @param key Member name.
     * @param fallback Returned if the member is missing or not bool/number.
     * @return The member's boolean value, or @p fallback.
     */
    bool bool_or(const std::string& key, bool fallback) const;

    /**
     * @brief Read a string member with a fallback.
     * @param key Member name.
     * @param fallback Returned if the member is missing or not a string.
     * @return The member's string, or @p fallback.
     */
    std::string str_or(const std::string& key, const std::string& fallback) const;
};

/**
 * @brief Parse a JSON document.
 * @param text The JSON text.
 * @return The root value, or nullptr on empty/invalid input.
 */
ValuePtr parse(const std::string& text);

/**
 * @brief Serialize a number as JSON.
 * @param value The number.
 * @return Integer form (no decimal point) for integral values; NaN/Inf become "null".
 */
std::string num_to_str(double value);

/**
 * @brief Quote and escape a string as a JSON string literal.
 * @param s Raw text.
 * @return The text wrapped in double quotes with control characters escaped.
 */
std::string quote(const std::string& s);

}  // namespace js
