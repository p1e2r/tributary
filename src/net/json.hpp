// SPDX-License-Identifier: MIT
// Small JSON value type with parser and serializer.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace json {

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v = nullptr;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v(b) {}
    Value(int i) : v(double(i)) {}
    Value(int64_t i) : v(double(i)) {}
    Value(double d) : v(d) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(Array a) : v(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : v(std::make_shared<Object>(std::move(o))) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool is_object() const { return std::holds_alternative<std::shared_ptr<Object>>(v); }
    bool is_array() const { return std::holds_alternative<std::shared_ptr<Array>>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }

    const Value& operator[](const std::string& k) const;
    const Value& operator[](size_t i) const;
    Value& set(const std::string& k, Value val);  // object only
    void push(Value val);                         // array only

    std::string str(const std::string& def = "") const;
    double num(double def = 0) const;
    int64_t integer(int64_t def = 0) const { return int64_t(num(double(def))); }
    bool boolean(bool def = false) const;
    const Array& arr() const;
    const Object& obj() const;
    size_t size() const;

    std::string dump(bool pretty = false, int indent = 0) const;
};

Value parse(const std::string& text);  // throws std::runtime_error

}  // namespace json
