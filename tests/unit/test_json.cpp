#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "utils/Json.h"

using videoeye::utils::JsonParse;
using videoeye::utils::JsonValue;

namespace {
// 解析一个典型的 QC 模板 JSON，校验取数路径与转义
TEST(JsonTest, ParseObjectAndAccessors) {
    const std::string text = R"({
        "version": 1,
        "id": "hls-vod",
        "name": "HLS VOD",
        "enabled": true,
        "threshold": 6.5,
        "tags": ["a", "b"],
        "nested": { "k": "中文与\u4e2d\u6587" }
    })";

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(text, root, &err)) << err;
    ASSERT_TRUE(root.IsObject());

    const JsonValue* id = root.Find("id");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->StringValueOr(), "hls-vod");
    EXPECT_TRUE(root.Has("enabled"));
    EXPECT_EQ(root.Find("enabled")->BoolValueOr(), true);
    EXPECT_NEAR(root.Find("threshold")->NumberValueOr(), 6.5, 1e-9);

    const JsonValue* tags = root.Find("tags");
    ASSERT_NE(tags, nullptr);
    ASSERT_TRUE(tags->IsArray());
    ASSERT_EQ(tags->Size(), 2u);
    EXPECT_EQ(tags->Items()[0].StringValueOr(), "a");
    EXPECT_EQ(tags->Items()[1].StringValueOr(), "b");

    const JsonValue* nested = root.Find("nested");
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->Find("k")->StringValueOr(), "中文与中文");
}

TEST(JsonTest, MissingKeyReturnsNull) {
    JsonValue root;
    ASSERT_TRUE(JsonParse(R"({"a":1})", root));
    EXPECT_EQ(root.Find("b"), nullptr);  // 缺失键返回 nullptr，不崩溃
    EXPECT_FALSE(root.Has("b"));
}

TEST(JsonTest, AccessorFallbackOnTypeMismatch) {
    JsonValue root;
    ASSERT_TRUE(JsonParse(R"({"a":1, "s":"x"})", root));
    // 类型不匹配时访问器回退默认值，不抛异常、不 UB
    EXPECT_EQ(root.Find("a")->StringValueOr("fallback"), "fallback");
    EXPECT_EQ(root.Find("s")->NumberValueOr(42), 42);
}

TEST(JsonTest, MalformedJsonFails) {
    JsonValue root;
    std::string err;
    EXPECT_FALSE(JsonParse(R"({"a": )", root, &err));
    EXPECT_FALSE(JsonParse(R"([1, 2,)", root, &err));
    EXPECT_TRUE(err.find("行") != std::string::npos || !err.empty());
}

TEST(JsonTest, RoundTripSerialization) {
    JsonValue obj = JsonValue::MakeObject();
    obj.Set("name", JsonValue("测试"));
    obj.Set("score", JsonValue(95.5));
    obj.Set("flag", JsonValue(true));
    JsonValue arr = JsonValue::MakeArray();
    arr.PushBack(JsonValue(1)).PushBack(JsonValue(2));
    obj.Set("list", std::move(arr));

    const std::string pretty = obj.ToPrettyString();
    JsonValue reparsed;
    ASSERT_TRUE(JsonParse(pretty, reparsed));
    EXPECT_EQ(reparsed.Find("name")->StringValueOr(), "测试");
    EXPECT_NEAR(reparsed.Find("score")->NumberValueOr(), 95.5, 1e-9);
    EXPECT_EQ(reparsed.Find("flag")->BoolValueOr(), true);
    EXPECT_EQ(reparsed.Find("list")->Size(), 2u);

    // 紧凑模式也应当是合法 JSON
    JsonValue compact_reparsed;
    ASSERT_TRUE(JsonParse(obj.ToString(), compact_reparsed));
    EXPECT_EQ(compact_reparsed.Find("list")->Size(), 2u);
}

TEST(JsonTest, NumberEdgeCases) {
    JsonValue root;
    ASSERT_TRUE(JsonParse(R"({"neg": -3.25, "zero": 0, "big": 123456})", root));
    EXPECT_NEAR(root.Find("neg")->NumberValueOr(), -3.25, 1e-9);
    EXPECT_NEAR(root.Find("zero")->NumberValueOr(), 0.0, 1e-9);
    EXPECT_EQ(root.Find("big")->IntValueOr(), 123456);
}
}  // namespace
