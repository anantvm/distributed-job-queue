#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// json_util.hpp
//
// Minimal self-contained JSON builder + parser for flat objects.
// Supports: string, int, int64_t, bool fields only (no nested objects/arrays).
// Sufficient for all Phase 2 wire-protocol messages — no external library needed.
// ─────────────────────────────────────────────────────────────────────────────

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>

namespace json_util {

// ─── Escaping ─────────────────────────────────────────────────────────────────

inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// ─── Builder ──────────────────────────────────────────────────────────────────

class JsonBuilder {
public:
    JsonBuilder& field(const std::string& key, const std::string& val) {
        sep(); oss_ << '"' << escape(key) << "\":\"" << escape(val) << '"';
        return *this;
    }
    JsonBuilder& field(const std::string& key, int val) {
        sep(); oss_ << '"' << escape(key) << "\":" << val;
        return *this;
    }
    JsonBuilder& field(const std::string& key, int64_t val) {
        sep(); oss_ << '"' << escape(key) << "\":" << val;
        return *this;
    }
    JsonBuilder& field(const std::string& key, bool val) {
        sep(); oss_ << '"' << escape(key) << "\":" << (val ? "true" : "false");
        return *this;
    }
    std::string build() const { return "{" + oss_.str() + "}"; }

private:
    std::ostringstream oss_;
    bool first_{true};
    void sep() { if (!first_) oss_ << ','; first_ = false; }
};

// ─── Parser helpers ───────────────────────────────────────────────────────────

inline std::string get_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        ++pos;
    }
    return result;
}

inline int64_t get_int64(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    bool neg = (pos < json.size() && json[pos] == '-');
    if (neg) ++pos;
    int64_t val = 0;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        val = val * 10 + static_cast<int64_t>(json[pos] - '0');
        ++pos;
    }
    return neg ? -val : val;
}

inline int get_int(const std::string& json, const std::string& key) {
    return static_cast<int>(get_int64(json, key));
}

inline bool get_bool(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    return json.size() >= pos + 4 && json.substr(pos, 4) == "true";
}

} // namespace json_util
