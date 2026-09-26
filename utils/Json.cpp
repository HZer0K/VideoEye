#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace videoeye {
namespace utils {
namespace {

constexpr int kMaxParseDepth = 64;

bool IsWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// UTF-16 代理对合成 codepoint 后再编码成 UTF-8（profile 里可能会有中文说明，
// 但字符串本身保持 UTF-8 字节序列就够了，不需要额外的宽字符库）
void AppendUtf8(std::string& out, unsigned int codepoint) {
    if (codepoint <= 0x7F) {
        out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

// 读 4 位十六进制，失败返回 false
bool ReadHex4(const std::string& text, std::size_t& pos, unsigned int& value) {
    if (pos + 4 > text.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = text[pos + i];
        unsigned int digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned int>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned int>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned int>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    pos += 4;
    return true;
}

void AppendNumber(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "null";  // JSON 只有 number，没有 NaN / Infinity
        return;
    }
    char buf[40];
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    } else {
        std::snprintf(buf, sizeof(buf), "%.10g", value);
    }
    out += buf;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool Parse(JsonValue& out, std::string* error) {
        SkipWhitespace();
        JsonValue value;
        if (!ParseValue(value, 0)) return Fail(error);
        SkipWhitespace();
        if (pos_ != text_.size()) return Fail(error, "尾部存在多余内容");
        out = std::move(value);
        return true;
    }

private:
    bool Fail(std::string* error, const char* reason = nullptr) {
        if (error != nullptr) {
            int line = 1;
            for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
                if (text_[i] == '\n') ++line;
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "第 %d 行附近解析失败", line);
            *error = reason != nullptr ? (std::string(buf) + ": " + reason) : std::string(buf);
        }
        return false;
    }

    void SkipWhitespace() {
        while (pos_ < text_.size() && IsWhitespace(text_[pos_])) ++pos_;
    }

    bool Expect(char c) {
        if (pos_ >= text_.size() || text_[pos_] != c) return false;
        ++pos_;
        return true;
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > kMaxParseDepth) return false;
        SkipWhitespace();
        if (pos_ >= text_.size()) return false;

        switch (text_[pos_]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': {
                std::string str;
                if (!ParseString(str)) return false;
                out = JsonValue(std::move(str));
                return true;
            }
            case 't':
                if (text_.compare(pos_, 4, "true") == 0) {
                    pos_ += 4;
                    out = JsonValue(true);
                    return true;
                }
                return false;
            case 'f':
                if (text_.compare(pos_, 5, "false") == 0) {
                    pos_ += 5;
                    out = JsonValue(false);
                    return true;
                }
                return false;
            case 'n':
                if (text_.compare(pos_, 4, "null") == 0) {
                    pos_ += 4;
                    out = JsonValue();
                    return true;
                }
                return false;
            default: return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out, int depth) {
        JsonValue object = JsonValue::MakeObject();
        ++pos_;  // '{'
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = std::move(object);
            return true;
        }
        while (true) {
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') return false;
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (!Expect(':')) return false;
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            object.Set(key, std::move(value));
            SkipWhitespace();
            if (Expect(',')) continue;
            if (Expect('}')) break;
            return false;
        }
        out = std::move(object);
        return true;
    }

    bool ParseArray(JsonValue& out, int depth) {
        JsonValue array = JsonValue::MakeArray();
        ++pos_;  // '['
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = std::move(array);
            return true;
        }
        while (true) {
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            array.PushBack(std::move(value));
            SkipWhitespace();
            if (Expect(',')) continue;
            if (Expect(']')) break;
            return false;
        }
        out = std::move(array);
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;  // 起始引号
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) return false;
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        unsigned int code = 0;
                        if (!ReadHex4(text_, pos_, code)) return false;
                        if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            const std::size_t save = pos_;
                            pos_ += 2;
                            unsigned int low = 0;
                            if (!ReadHex4(text_, pos_, low) || low < 0xDC00 || low > 0xDFFF) {
                                pos_ = save;  // 不是合法代理对，按原字符处理
                            } else {
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            }
                        }
                        AppendUtf8(out, code);
                        break;
                    }
                    default: return false;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) return false;  // 未转义的控制字符
            out += c;
            ++pos_;
        }
        return false;
    }

    bool ParseNumber(JsonValue& out) {
        const std::size_t begin = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool digits = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                digits = true;
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++pos_;
            } else {
                break;
            }
        }
        if (!digits) return false;
        out = JsonValue(std::strtod(text_.substr(begin, pos_ - begin).c_str(), nullptr));
        return true;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

}  // namespace

JsonValue::JsonValue(bool value) : type_(JsonType::Bool), bool_value_(value) {}
JsonValue::JsonValue(double value) : type_(JsonType::Number), number_value_(value) {}
JsonValue::JsonValue(int value)
    : type_(JsonType::Number), number_value_(static_cast<double>(value)) {}
JsonValue::JsonValue(std::string value) : type_(JsonType::String), string_value_(std::move(value)) {}
JsonValue::JsonValue(const char* value) : type_(JsonType::String), string_value_(value ? value : "") {}

JsonValue JsonValue::MakeArray() {
    JsonValue value;
    value.type_ = JsonType::Array;
    return value;
}

JsonValue JsonValue::MakeObject() {
    JsonValue value;
    value.type_ = JsonType::Object;
    return value;
}

bool JsonValue::BoolValueOr(bool fallback) const {
    return type_ == JsonType::Bool ? bool_value_ : fallback;
}

double JsonValue::NumberValueOr(double fallback) const {
    return type_ == JsonType::Number ? number_value_ : fallback;
}

int JsonValue::IntValueOr(int fallback) const {
    if (type_ != JsonType::Number) return fallback;
    return static_cast<int>(number_value_);
}

std::string JsonValue::StringValueOr(const std::string& fallback) const {
    return type_ == JsonType::String ? string_value_ : fallback;
}

JsonValue& JsonValue::PushBack(JsonValue value) {
    type_ = JsonType::Array;
    items_.push_back(std::move(value));
    return *this;
}

const JsonValue* JsonValue::Find(const std::string& key) const {
    if (type_ != JsonType::Object) return nullptr;
    for (std::size_t i = 0; i < member_keys_.size(); ++i) {
        if (member_keys_[i] == key) return &member_values_[i];
    }
    return nullptr;
}

JsonValue* JsonValue::FindMutable(const std::string& key) {
    if (type_ != JsonType::Object) return nullptr;
    for (std::size_t i = 0; i < member_keys_.size(); ++i) {
        if (member_keys_[i] == key) return &member_values_[i];
    }
    return nullptr;
}

JsonValue& JsonValue::Set(const std::string& key, JsonValue value) {
    type_ = JsonType::Object;
    if (JsonValue* existing = FindMutable(key)) {
        *existing = std::move(value);
        return *existing;
    }
    member_keys_.push_back(key);
    member_values_.push_back(std::move(value));
    return member_values_.back();
}

std::string JsonValue::ToString() const {
    std::string out;
    WriteTo(out, -1, 0);
    return out;
}

std::string JsonValue::ToPrettyString(int indent) const {
    std::string out;
    WriteTo(out, indent, 0);
    return out;
}

void JsonValue::WriteTo(std::string& out, int step, int depth) const {
    const bool pretty = step > 0;
    auto newline = [&](int level) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<std::size_t>(level) * static_cast<std::size_t>(step), ' ');
    };

    switch (type_) {
        case JsonType::Null:   out += "null"; break;
        case JsonType::Bool:   out += bool_value_ ? "true" : "false"; break;
        case JsonType::Number: AppendNumber(out, number_value_); break;
        case JsonType::String: out += JsonEscape(string_value_); break;
        case JsonType::Array: {
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i > 0) out += ',';
                newline(depth + 1);
                items_[i].WriteTo(out, step, depth + 1);
            }
            newline(depth);
            out += ']';
            break;
        }
        case JsonType::Object: {
            out += '{';
            for (std::size_t i = 0; i < member_keys_.size(); ++i) {
                if (i > 0) out += ',';
                newline(depth + 1);
                out += JsonEscape(member_keys_[i]);
                out += ':';
                if (pretty) out += ' ';
                member_values_[i].WriteTo(out, step, depth + 1);
            }
            newline(depth);
            out += '}';
            break;
        }
    }
}

bool JsonParse(const std::string& text, JsonValue& out, std::string* error_out) {
    JsonParser parser(text);
    return parser.Parse(out, error_out);
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out += '"';
    for (unsigned char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
    return out;
}

}  // namespace utils
}  // namespace videoeye
