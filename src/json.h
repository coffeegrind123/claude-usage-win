#pragma once
// A small JSON DOM: enough for status.json, samples.jsonl, the provider APIs and the transcripts, with
// no dependency. Objects keep insertion order, as JSON.stringify does, so the files read
// the same to anything else that parses them.
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
public:
    using Member = std::pair<std::string, Value>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    template <class T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T n) : type_(Type::Number), n_(static_cast<double>(n)) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), s_(s) {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNum() const { return type_ == Type::Number; }
    bool isStr() const { return type_ == Type::String; }
    bool isArr() const { return type_ == Type::Array; }
    bool isObj() const { return type_ == Type::Object; }

    // Lenient reads: a missing key or a wrong type gives the fallback — the `a?.b ?? x` of the JS original.
    const Value& operator[](std::string_view key) const;
    const Value& operator[](size_t i) const;
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool has(std::string_view key) const { return find(key) != nullptr; }
    double num(double fallback = 0) const { return type_ == Type::Number ? n_ : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? b_ : fallback; }
    const std::string& str() const;
    std::string str(std::string_view fallback) const { return type_ == Type::String ? s_ : std::string(fallback); }
    /// JavaScript truthiness: what `if (x)` and `x || y` test.
    bool truthy() const;

    size_t size() const;
    const std::vector<Value>& items() const { return a_; }
    std::vector<Value>& items() { return a_; }
    const std::vector<Member>& members() const { return o_; }
    std::vector<Member>& members() { return o_; }

    /// Object: replaces the value in place (keeping its position) or appends. Turns null into an object.
    Value& set(std::string_view key, Value v);
    bool erase(std::string_view key);
    /// Array: appends. Turns null into an array.
    Value& push(Value v);

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> a_;
    std::vector<Member> o_;
};

/// Strict RFC 8259 parse of the whole text (surrounding whitespace allowed). False on any error.
bool parse(std::string_view text, Value& out);
/// indent 0 = compact (a JSONL line); 2 = JSON.stringify(v, null, 2). Non-finite numbers write null.
std::string dump(const Value& v, int indent = 0);
/// Number#toString: integral values without a fraction, the shortest round-trip digits otherwise.
std::string number(double n);

}  // namespace json
