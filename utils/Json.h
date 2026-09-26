#pragma once

// 极简 JSON 读写实现（C++17，不依赖 Qt / FFmpeg / 第三方库）
//
// 为什么不用现成库：项目对外部依赖控制很严（硬依赖只有 Qt Widgets 与 FFmpeg），
// 为"读一个 QC 模板配置"引入 json.hpp 不划算。这里只覆盖配置读写需要的子集：
//   * 解析：对象 / 数组 / 字符串 / 数字 / true / false / null，支持 \uXXXX 转义与代理对
//   * 序列化：紧凑模式（写文件）与缩进模式（人工查看）
//
// 数值统一用 double 承载（模板阈值都是标量），序列化时对 NaN/Inf 退化成 null ——
// JSON 规范本身不允许这三种字面量，写出去只会在下游解析器报错。
//
// 注意：对象成员的键与值分别存在两个平行数组里（member_keys_ / member_values_），
// 这是为了避免 JsonValue 自引用的完整类型问题（JsonMember{key, JsonValue} 做不到）。

#include <cstddef>
#include <string>
#include <vector>

namespace videoeye {
namespace utils {

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

class JsonValue {
public:
    JsonValue() = default;                       // Null
    explicit JsonValue(bool value);
    explicit JsonValue(double value);
    explicit JsonValue(int value);
    explicit JsonValue(std::string value);
    explicit JsonValue(const char* value);

    // ---- 工厂：构造除 Null 以外的值时语义更清楚 ----
    static JsonValue MakeArray();
    static JsonValue MakeObject();

    JsonType type() const { return type_; }
    bool IsNull() const { return type_ == JsonType::Null; }
    bool IsBool() const { return type_ == JsonType::Bool; }
    bool IsNumber() const { return type_ == JsonType::Number; }
    bool IsString() const { return type_ == JsonType::String; }
    bool IsArray() const { return type_ == JsonType::Array; }
    bool IsObject() const { return type_ == JsonType::Object; }

    // ---- 取值：类型不匹配时返回默认参数，绝不抛异常（配置文件可读性是第一位）----
    bool BoolValueOr(bool fallback = false) const;
    double NumberValueOr(double fallback = 0.0) const;
    int IntValueOr(int fallback = 0) const;
    std::string StringValueOr(const std::string& fallback = "") const;

    // ---- 数组 ----
    const std::vector<JsonValue>& Items() const { return items_; }
    std::size_t Size() const { return items_.size(); }
    JsonValue& PushBack(JsonValue value);

    // ---- 对象 ----
    const std::vector<std::string>& MemberKeys() const { return member_keys_; }
    const std::vector<JsonValue>& MemberValues() const { return member_values_; }
    // 未找到返回 nullptr（调用方据此判断"键不存在"与"键存在但值是 null"）
    const JsonValue* Find(const std::string& key) const;
    JsonValue* FindMutable(const std::string& key);
    bool Has(const std::string& key) const { return Find(key) != nullptr; }
    JsonValue& Set(const std::string& key, JsonValue value);

    // ---- 序列化 ----
    std::string ToString() const;                     // 紧凑
    std::string ToPrettyString(int indent = 2) const; // 缩进

private:
    void WriteTo(std::string& out, int indent, int depth) const;

    JsonType type_ = JsonType::Null;
    bool bool_value_ = false;
    double number_value_ = 0.0;
    std::string string_value_;
    std::vector<JsonValue> items_;
    std::vector<std::string> member_keys_;
    std::vector<JsonValue> member_values_;
};

// 解析整个文本。失败时返回 false 并通过 error_out（可选）给出带行号的原因。
bool JsonParse(const std::string& text, JsonValue& out, std::string* error_out = nullptr);

// 序列化时对字符串做 JSON 转义（双引号 / 反斜杠 / 控制字符），非 ASCII 原样输出 UTF-8。
std::string JsonEscape(const std::string& text);

}  // namespace utils
}  // namespace videoeye
