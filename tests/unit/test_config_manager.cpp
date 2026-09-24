#include <gtest/gtest.h>

#include "utils/ConfigManager.h"

namespace {

TEST(ConfigManagerTest, StoresTypeValuesAndDefaults) {
    auto& config = videoeye::utils::ConfigManager::GetInstance();
    config.Clear();

    config.SetString("ui.theme", "dark");
    config.SetInt("analysis.window", 60);
    config.SetDouble("playback.speed", 1.0);
    config.SetBool("render.vulkan", true);

    EXPECT_EQ(config.GetString("ui.theme"), "dark");
    EXPECT_EQ(config.GetInt("analysis.window"), 60);
    EXPECT_DOUBLE_EQ(config.GetDouble("playback.speed"), 1.0);
    EXPECT_TRUE(config.GetBool("render.vulkan"));
    EXPECT_EQ(config.GetString("missing", "fallback"), "fallback");

    config.RemoveKey("ui.theme");
    EXPECT_FALSE(config.HasKey("ui.theme"));

    config.Clear();
}

} // namespace