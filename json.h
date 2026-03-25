#pragma once

#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace json {

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    using Data = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Data data = nullptr;

    Value() = default;
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool b) : data(b) {}
    Value(int n) : data(static_cast<double>(n)) {}
    Value(size_t n) : data(static_cast<double>(n)) {}
    Value(double n) : data(n) {}
    Value(const char* s) : data(std::string(s)) {}
    Value(std::string s) : data(std::move(s)) {}
    Value(Array a) : data(std::move(a)) {}
    Value(Object o) : data(std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_num() const { return std::holds_alternative<double>(data); }
    bool is_str() const { return std::holds_alternative<std::string>(data); }
    bool is_arr() const { return std::holds_alternative<Array>(data); }
    bool is_obj() const { return std::holds_alternative<Object>(data); }

    bool as_bool() const { return std::get<bool>(data); }
    double as_num() const { return std::get<double>(data); }
    int as_int() const { return static_cast<int>(std::get<double>(data)); }
    const std::string& as_str() const { return std::get<std::string>(data); }
    const Array& as_arr() const { return std::get<Array>(data); }
    Array& as_arr() { return std::get<Array>(data); }
    const Object& as_obj() const { return std::get<Object>(data); }
    Object& as_obj() { return std::get<Object>(data); }

    Value& operator[](const std::string& key) { return std::get<Object>(data)[key]; }
    const Value& at(const std::string& key) const { return std::get<Object>(data).at(key); }
    bool has(const std::string& key) const {
        if (!is_obj()) return false;
        return as_obj().count(key) > 0;
    }

    Value& operator[](size_t i) { return std::get<Array>(data)[i]; }
    const Value& at(size_t i) const { return std::get<Array>(data).at(i); }

    std::string str_or(const std::string& key, const std::string& def) const {
        if (has(key) && at(key).is_str()) return at(key).as_str();
        return def;
    }
    int int_or(const std::string& key, int def) const {
        if (has(key) && at(key).is_num()) return at(key).as_int();
        return def;
    }
    bool bool_or(const std::string& key, bool def) const {
        if (has(key) && at(key).is_bool()) return at(key).as_bool();
        return def;
    }
};

// ── Parser ──────────────────────────────────────────────────

class Parser {
    const std::string& src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size() && (src[pos]==' ' || src[pos]=='\n' || src[pos]=='\r' || src[pos]=='\t'))
            ++pos;
    }
    char peek() { return pos < src.size() ? src[pos] : '\0'; }
    char next() { return pos < src.size() ? src[pos++] : '\0'; }
    void expect(char c) {
        skip_ws();
        if (next() != c)
            throw std::runtime_error(std::string("JSON: expected '") + c + "'");
    }

    std::string parse_string() {
        expect('"');
        std::string s;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') {
                ++pos;
                if (pos >= src.size()) break;
                switch (src[pos]) {
                    case '"':  s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/':  s += '/'; break;
                    case 'n':  s += '\n'; break;
                    case 'r':  s += '\r'; break;
                    case 't':  s += '\t'; break;
                    case 'b':  s += '\b'; break;
                    case 'f':  s += '\f'; break;
                    default:   s += src[pos]; break;
                }
            } else {
                s += src[pos];
            }
            ++pos;
        }
        if (pos < src.size()) ++pos; // closing quote
        return s;
    }

    Value parse_number() {
        size_t start = pos;
        if (pos < src.size() && src[pos] == '-') ++pos;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
        if (pos < src.size() && src[pos] == '.') {
            ++pos;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            ++pos;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }
        return Value(std::stod(src.substr(start, pos - start)));
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        if (c == '"') return Value(parse_string());
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't') { pos += 4; return Value(true); }
        if (c == 'f') { pos += 5; return Value(false); }
        if (c == 'n') { pos += 4; return Value(nullptr); }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        throw std::runtime_error("JSON: unexpected character");
    }

    Value parse_object() {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() != '}') {
            while (true) {
                std::string key = parse_string();
                expect(':');
                obj[key] = parse_value();
                skip_ws();
                if (peek() == ',') { ++pos; continue; }
                break;
            }
        }
        expect('}');
        return Value(std::move(obj));
    }

    Value parse_array() {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() != ']') {
            while (true) {
                arr.push_back(parse_value());
                skip_ws();
                if (peek() == ',') { ++pos; continue; }
                break;
            }
        }
        expect(']');
        return Value(std::move(arr));
    }

public:
    Parser(const std::string& s) : src(s) {}
    Value parse() { return parse_value(); }
};

inline Value parse(const std::string& s) {
    Parser p(s);
    return p.parse();
}

// ── Serializer ──────────────────────────────────────────────

inline std::string escape_str(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 2);
    r += '"';
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:   r += c;
        }
    }
    r += '"';
    return r;
}

inline void serialize_impl(const Value& v, std::ostringstream& os, int indent, int depth) {
    std::string pad(depth * indent, ' ');
    std::string pad1((depth + 1) * indent, ' ');

    if (v.is_null()) { os << "null"; }
    else if (v.is_bool()) { os << (v.as_bool() ? "true" : "false"); }
    else if (v.is_num()) {
        double n = v.as_num();
        if (n == static_cast<long long>(n) && n >= -1e15 && n <= 1e15)
            os << static_cast<long long>(n);
        else
            os << n;
    }
    else if (v.is_str()) { os << escape_str(v.as_str()); }
    else if (v.is_arr()) {
        auto& a = v.as_arr();
        if (a.empty()) { os << "[]"; return; }
        os << "[\n";
        for (size_t i = 0; i < a.size(); ++i) {
            os << pad1;
            serialize_impl(a[i], os, indent, depth + 1);
            if (i + 1 < a.size()) os << ",";
            os << "\n";
        }
        os << pad << "]";
    }
    else if (v.is_obj()) {
        auto& o = v.as_obj();
        if (o.empty()) { os << "{}"; return; }
        os << "{\n";
        size_t i = 0;
        for (auto& [k, val] : o) {
            os << pad1 << escape_str(k) << ": ";
            serialize_impl(val, os, indent, depth + 1);
            if (++i < o.size()) os << ",";
            os << "\n";
        }
        os << pad << "}";
    }
}

inline std::string serialize(const Value& v, int indent = 2) {
    std::ostringstream os;
    serialize_impl(v, os, indent, 0);
    os << "\n";
    return os.str();
}

} // namespace json
