#include "json.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

namespace json {

namespace {

const Value kNull;
const std::string kEmpty;

// Transcript lines nest a few dozen levels at most; the cap only keeps hostile input off the stack.
constexpr int MAX_DEPTH = 512;

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    bool run(Value& out) {
        ws();
        if (!value(out, 0)) {
            return false;
        }
        ws();
        return p_ == t_.size();
    }

private:
    std::string_view t_;
    size_t p_ = 0;

    void ws() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) {
            p_++;
        }
    }

    bool lit(std::string_view word) {
        if (t_.substr(p_, word.size()) != word) {
            return false;
        }
        p_ += word.size();
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > MAX_DEPTH || p_ >= t_.size()) {
            return false;
        }
        switch (t_[p_]) {
        case 'n': out = Value(); return lit("null");
        case 't': out = Value(true); return lit("true");
        case 'f': out = Value(false); return lit("false");
        case '"': {
            std::string s;
            if (!string(s)) {
                return false;
            }
            out = Value(std::move(s));
            return true;
        }
        case '[': return array(out, depth);
        case '{': return object(out, depth);
        default: return numberValue(out);
        }
    }

    bool numberValue(Value& out) {
        size_t start = p_;
        if (p_ < t_.size() && t_[p_] == '-') {
            p_++;
        }
        if (p_ >= t_.size() || !isdigit(static_cast<unsigned char>(t_[p_]))) {
            return false;
        }
        if (t_[p_] == '0') {
            p_++;
        } else {
            while (p_ < t_.size() && isdigit(static_cast<unsigned char>(t_[p_]))) {
                p_++;
            }
        }
        if (p_ < t_.size() && t_[p_] == '.') {
            p_++;
            if (p_ >= t_.size() || !isdigit(static_cast<unsigned char>(t_[p_]))) {
                return false;
            }
            while (p_ < t_.size() && isdigit(static_cast<unsigned char>(t_[p_]))) {
                p_++;
            }
        }
        if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
            p_++;
            if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) {
                p_++;
            }
            if (p_ >= t_.size() || !isdigit(static_cast<unsigned char>(t_[p_]))) {
                return false;
            }
            while (p_ < t_.size() && isdigit(static_cast<unsigned char>(t_[p_]))) {
                p_++;
            }
        }
        double d = 0;
        auto r = std::from_chars(t_.data() + start, t_.data() + p_, d);
        if (r.ec == std::errc::result_out_of_range) {
            // JSON.parse turns an overflowing literal into ±Infinity rather than failing the document.
            d = t_[start] == '-' ? -HUGE_VAL : HUGE_VAL;
        } else if (r.ec != std::errc()) {
            return false;
        }
        out = Value(d);
        return true;
    }

    static void putUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& v) {
        if (p_ + 4 > t_.size()) {
            return false;
        }
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool string(std::string& s) {
        p_++;   // the opening quote
        // Fast path: copy runs without escapes in one go — transcript lines are mostly long plain strings.
        while (p_ < t_.size()) {
            size_t run = p_;
            while (run < t_.size() && t_[run] != '"' && t_[run] != '\\' && static_cast<unsigned char>(t_[run]) >= 0x20) {
                run++;
            }
            s.append(t_.data() + p_, run - p_);
            p_ = run;
            if (p_ >= t_.size()) {
                return false;
            }
            char c = t_[p_];
            if (c == '"') {
                p_++;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            p_++;   // backslash
            if (p_ >= t_.size()) {
                return false;
            }
            char e = t_[p_++];
            switch (e) {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 6 <= t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                    size_t save = p_;
                    p_ += 2;
                    uint32_t lo = 0;
                    if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p_ = save;
                    }
                }
                // A lone surrogate is legal JSON but not UTF-8: write U+FFFD like a lossy decoder would.
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                putUtf8(s, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool array(Value& out, int depth) {
        p_++;
        out = Value::array();
        ws();
        if (p_ < t_.size() && t_[p_] == ']') {
            p_++;
            return true;
        }
        while (true) {
            ws();
            Value v;
            if (!value(v, depth + 1)) {
                return false;
            }
            out.items().push_back(std::move(v));
            ws();
            if (p_ >= t_.size()) {
                return false;
            }
            if (t_[p_] == ',') {
                p_++;
                continue;
            }
            if (t_[p_] == ']') {
                p_++;
                return true;
            }
            return false;
        }
    }

    bool object(Value& out, int depth) {
        p_++;
        out = Value::object();
        ws();
        if (p_ < t_.size() && t_[p_] == '}') {
            p_++;
            return true;
        }
        while (true) {
            ws();
            if (p_ >= t_.size() || t_[p_] != '"') {
                return false;
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            ws();
            if (p_ >= t_.size() || t_[p_] != ':') {
                return false;
            }
            p_++;
            ws();
            Value v;
            if (!value(v, depth + 1)) {
                return false;
            }
            // A duplicate key: the last one wins, in the first one's place — what JSON.parse does.
            out.set(key, std::move(v));
            ws();
            if (p_ >= t_.size()) {
                return false;
            }
            if (t_[p_] == ',') {
                p_++;
                continue;
            }
            if (t_[p_] == '}') {
                p_++;
                return true;
            }
            return false;
        }
    }
};

void escape(std::string& out, const std::string& s) {
    static const char* hex = "0123456789abcdef";
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void write(std::string& out, const Value& v, int indent, int level) {
    auto newline = [&](int lv) {
        if (!indent) {
            return;
        }
        out += '\n';
        out.append(static_cast<size_t>(indent * lv), ' ');
    };
    switch (v.type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += v.boolean() ? "true" : "false"; break;
    case Type::Number: out += std::isfinite(v.num()) ? number(v.num()) : "null"; break;
    case Type::String: escape(out, v.str()); break;
    case Type::Array: {
        if (v.items().empty()) {
            out += "[]";
            break;
        }
        out += '[';
        bool first = true;
        for (const Value& x : v.items()) {
            if (!first) {
                out += ',';
            }
            first = false;
            newline(level + 1);
            write(out, x, indent, level + 1);
        }
        newline(level);
        out += ']';
        break;
    }
    case Type::Object: {
        if (v.members().empty()) {
            out += "{}";
            break;
        }
        out += '{';
        bool first = true;
        for (const auto& [k, x] : v.members()) {
            if (!first) {
                out += ',';
            }
            first = false;
            newline(level + 1);
            escape(out, k);
            out += indent ? ": " : ":";
            write(out, x, indent, level + 1);
        }
        newline(level);
        out += '}';
        break;
    }
    }
}

}  // namespace

const Value& Value::operator[](std::string_view key) const {
    const Value* v = find(key);
    return v ? *v : kNull;
}

const Value& Value::operator[](size_t i) const {
    return type_ == Type::Array && i < a_.size() ? a_[i] : kNull;
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& m : o_) {
        if (m.first == key) {
            return &m.second;
        }
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    return const_cast<Value*>(static_cast<const Value*>(this)->find(key));
}

const std::string& Value::str() const {
    return type_ == Type::String ? s_ : kEmpty;
}

bool Value::truthy() const {
    switch (type_) {
    case Type::Null: return false;
    case Type::Bool: return b_;
    case Type::Number: return n_ != 0 && !std::isnan(n_);
    case Type::String: return !s_.empty();
    default: return true;
    }
}

size_t Value::size() const {
    return type_ == Type::Array ? a_.size() : type_ == Type::Object ? o_.size() : 0;
}

Value& Value::set(std::string_view key, Value v) {
    if (type_ != Type::Object) {
        *this = object();
    }
    for (auto& m : o_) {
        if (m.first == key) {
            m.second = std::move(v);
            return m.second;
        }
    }
    o_.emplace_back(std::string(key), std::move(v));
    return o_.back().second;
}

bool Value::erase(std::string_view key) {
    for (auto it = o_.begin(); it != o_.end(); ++it) {
        if (it->first == key) {
            o_.erase(it);
            return true;
        }
    }
    return false;
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) {
        *this = array();
    }
    a_.push_back(std::move(v));
    return a_.back();
}

bool parse(std::string_view text, Value& out) {
    // A UTF-8 BOM is not JSON, but a file saved by a Windows editor may carry one.
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    Parser p(text);
    Value v;
    if (!p.run(v)) {
        return false;
    }
    out = std::move(v);
    return true;
}

std::string dump(const Value& v, int indent) {
    std::string out;
    write(out, v, indent, 0);
    return out;
}

// ECMA-262 Number::toString(10): shortest round-trip digits, fixed notation for 1e-7 <= |n| < 1e21,
// exponential outside it ("1e+21", "1.5e-7").
std::string number(double n) {
    if (std::isnan(n)) {
        return "NaN";
    }
    if (std::isinf(n)) {
        return n < 0 ? "-Infinity" : "Infinity";
    }
    if (n == 0) {
        return "0";
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, n, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    std::string sign;
    if (sci[0] == '-') {
        sign = "-";
        sci.erase(0, 1);
    }
    size_t e = sci.find('e');
    std::string mant = sci.substr(0, e);
    int exp10 = std::stoi(sci.substr(e + 1));
    std::string digits;
    for (char c : mant) {
        if (c != '.') {
            digits += c;
        }
    }
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
    }
    int k = static_cast<int>(digits.size());
    int pointAt = exp10 + 1;   // ECMA's `n`: where the decimal point falls in the digit string
    std::string out;
    if (k <= pointAt && pointAt <= 21) {
        out = digits + std::string(static_cast<size_t>(pointAt - k), '0');
    } else if (0 < pointAt && pointAt <= 21) {
        out = digits.substr(0, static_cast<size_t>(pointAt)) + "." + digits.substr(static_cast<size_t>(pointAt));
    } else if (-6 < pointAt && pointAt <= 0) {
        out = "0." + std::string(static_cast<size_t>(-pointAt), '0') + digits;
    } else {
        int ex = pointAt - 1;
        out = digits.substr(0, 1) + (k > 1 ? "." + digits.substr(1) : "") + "e" + (ex < 0 ? "-" : "+") + std::to_string(ex < 0 ? -ex : ex);
    }
    return sign + out;
}

}  // namespace json
