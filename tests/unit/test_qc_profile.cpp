#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "core/qc/QcProfile.h"

using videoeye::qc::BuiltinQcProfiles;
using videoeye::qc::FindBuiltinQcProfile;
using videoeye::qc::LoadQcProfileFromFile;
using videoeye::qc::ParseQcProfileJson;
using videoeye::qc::QcProfile;
using videoeye::qc::SaveQcProfileToFile;
using videoeye::qc::SerializeQcProfile;
using videoeye::qc::UnknownRuleIds;

namespace {
TEST(QcProfileTest, BuiltinsArePresent) {
    const auto profiles = BuiltinQcProfiles();
    // 12.4 要求的 5 个内置模板
    ASSERT_EQ(profiles.size(), 5u);

    std::vector<std::string> ids;
    for (const auto& p : profiles) ids.push_back(p.id);
    EXPECT_EQ(ids[0], "general");
    EXPECT_EQ(ids[1], "broadcast");
    EXPECT_EQ(ids[2], "hls-vod");
    EXPECT_EQ(ids[3], "short-video");
    EXPECT_EQ(ids[4], "archive-master");
}

TEST(QcProfileTest, BuiltinIdsResolve) {
    for (const auto& p : BuiltinQcProfiles()) {
        const QcProfile* found = FindBuiltinQcProfile(p.id);
        ASSERT_TRUE(found != nullptr) << p.id;
        EXPECT_EQ(found->name, p.name);
        EXPECT_FALSE(found->name.empty());
    }
    EXPECT_TRUE(FindBuiltinQcProfile("does-not-exist") == nullptr);

    // 非通用的内置模板必须带非空覆盖项（通用模板按设计是空覆盖项，沿用默认规则）
    const QcProfile* broadcast = FindBuiltinQcProfile("broadcast");
    ASSERT_TRUE(broadcast != nullptr);
    EXPECT_FALSE(broadcast->overrides.empty());
}

TEST(QcProfileTest, RoundTripSerialization) {
    QcProfile profile = BuiltinQcProfiles()[2];  // hls-vod
    const std::string json = SerializeQcProfile(profile, /*pretty=*/true);

    QcProfile reparsed;
    std::string err;
    ASSERT_TRUE(ParseQcProfileJson(json, reparsed, err)) << err;
    EXPECT_EQ(reparsed.id, profile.id);
    EXPECT_EQ(reparsed.name, profile.name);
    EXPECT_EQ(reparsed.depth, profile.depth);
    EXPECT_EQ(reparsed.overrides.size(), profile.overrides.size());
}

TEST(QcProfileTest, OverrideApplyChangesThreshold) {
    QcProfile profile = BuiltinQcProfiles()[1];  // broadcast：确实带 video.gop.max_seconds 覆盖项
    // 改一个 gop 规则的阈值，确认序列化后能区分"已覆盖"与"未覆盖"
    bool found = false;
    for (auto& o : profile.overrides) {
        if (o.id == "video.gop.max_seconds") {
            o.has_enabled = true;
            o.enabled = true;
            o.has_threshold = true;
            o.threshold = 12.0;  // 与默认不同
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found) << "内置模板应含 video.gop.max_seconds 规则";

    const std::string json = SerializeQcProfile(profile);
    QcProfile reparsed;
    std::string err;
    ASSERT_TRUE(ParseQcProfileJson(json, reparsed, err)) << err;
    bool ok = false;
    for (const auto& o : reparsed.overrides) {
        if (o.id == "video.gop.max_seconds") {
            EXPECT_TRUE(o.has_threshold);
            EXPECT_NEAR(o.threshold, 12.0, 1e-9);
            ok = true;
        }
    }
    EXPECT_TRUE(ok);
}

TEST(QcProfileTest, UnknownRuleIdsDetected) {
    QcProfile profile = BuiltinQcProfiles()[0];
    // 注入一个不存在的规则 id
    videoeye::qc::QcRuleOverride bad;
    bad.id = "totally.made.up.rule";
    bad.has_enabled = true;
    bad.enabled = false;
    profile.overrides.push_back(bad);

    const auto unknown = UnknownRuleIds(profile);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown[0], "totally.made.up.rule");
}

TEST(QcProfileTest, FileRoundTrip) {
    QcProfile profile = BuiltinQcProfiles()[3];  // short-video
    const std::string path = std::filesystem::temp_directory_path().string() +
                             "/videoeye_qc_profile_test.json";
    ASSERT_TRUE(SaveQcProfileToFile(path, profile));

    QcProfile loaded;
    std::string err;
    ASSERT_TRUE(LoadQcProfileFromFile(path, loaded, err)) << err;
    EXPECT_EQ(loaded.id, profile.id);
    EXPECT_EQ(loaded.overrides.size(), profile.overrides.size());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
}  // namespace
