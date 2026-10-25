#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "infrastructure/config/ConfigManager.h"

namespace {

TEST(ConfigManagerTest, StoresTypeValuesAndDefaults) {
    auto& config = videoeye::ConfigManager::GetInstance();
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

// 并发保存：同一文件的多次保存不能互相截断、不能留下读不出的半截文件。
//
// 缺陷(已修): 文件 IO 从快照锁挪出去之后，同一文件的并发保存共用同一个
// <target>.tmp —— 两个线程同时打开它、互相截断；再加上"先把目标改名成 .bak、
// 再把 .tmp rename 上去"这两步之间的窗口，失败后目标文件还可能根本不存在。
// 实测 6 线程 / 600 次保存会失败 332 次。
// 修法: 保存操作锁串行化 + 唯一临时文件名 + 平台原子替换（不留缺失窗口）。
TEST(ConfigManagerTest, ConcurrentSavesNeverCorruptTheFile) {
    auto& config = videoeye::ConfigManager::GetInstance();
    config.Clear();

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "videoeye_config_concurrent.ini";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::filesystem::path(path.string() + ".bak"), ec);

    constexpr int kThreads = 6;
    constexpr int kSavesPerThread = 100;
    std::atomic<int> save_failures{0};
    std::atomic<int> unreadable{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kSavesPerThread; ++i) {
                config.SetString("thread." + std::to_string(t), std::to_string(i));
                if (!config.SaveToFile(path.string())) {
                    save_failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                // 每一次保存之后，文件都必须是一份**完整可读**的配置
                // （半截文件里不会有 schema 版本键）
                std::ifstream in(path, std::ios::binary);
                const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (content.find(videoeye::kSchemaVersionKey) == std::string::npos) {
                    unreadable.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads)
        thread.join();

    EXPECT_EQ(save_failures.load(), 0) << "并发保存失败次数（老实现 6 线程 / 600 次保存失败 332 次）";
    EXPECT_EQ(unreadable.load(), 0) << "保存后文件不可解析（半截内容）的次数";

    // 最终文件必须同时含有全部线程的键 —— 说明没有一次保存把别人的内容顶掉
    std::ifstream in(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_NE(content.find("thread." + std::to_string(t)), std::string::npos) << "线程 " << t << " 写入的键丢失了";
    }

    // 临时文件必须被收干净（唯一文件名也不能泄漏在临时目录里）
    int leftover = 0;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path(), ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("videoeye_config_concurrent.ini", 0) == 0 && name.find(".tmp") != std::string::npos) {
            ++leftover;
        }
    }
    EXPECT_EQ(leftover, 0) << "保存后残留了临时文件";

    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::filesystem::path(path.string() + ".bak"), ec);
    config.Clear();
}

// 保存失败不能把已经存在的好文件带走：替换失败时目标必须原封不动。
//
// 老实现是"先把目标改成 .bak、再移动 .tmp"，中间目标文件不存在，失败后也没人
// 把 .bak 恢复回去 —— 一次失败的保存会让配置凭空消失。
TEST(ConfigManagerTest, FailedSaveLeavesExistingFileIntact) {
    auto& config = videoeye::ConfigManager::GetInstance();
    config.Clear();
    config.SetString("probe.key", "probe");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "videoeye_config_keep.ini";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    ASSERT_TRUE(config.SaveToFile(path.string()));

    // 把目标换成"一个目录"：临时文件写得出来，但原子替换必然失败。
    std::filesystem::remove(path, ec);
    std::filesystem::create_directory(path, ec);
    ASSERT_TRUE(std::filesystem::is_directory(path, ec));

    config.SetString("probe.key", "changed");
    EXPECT_FALSE(config.SaveToFile(path.string())) << "目标是目录时保存必须失败";
    EXPECT_TRUE(std::filesystem::is_directory(path, ec)) << "失败后目标不能被悄悄删掉";

    std::filesystem::remove_all(path, ec);
    config.Clear();
}

// 正常路径下的保存仍然要成功，且能原样读回来（并发加固不能把基本功能改坏）。
TEST(ConfigManagerTest, SaveThenLoadRoundTrip) {
    auto& config = videoeye::ConfigManager::GetInstance();
    config.Clear();
    config.SetString("round.trip", "value");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "videoeye_config_roundtrip.ini";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ASSERT_TRUE(config.SaveToFile(path.string()));

    config.Clear();
    ASSERT_TRUE(config.LoadFromFile(path.string()));
    EXPECT_EQ(config.GetString("round.trip"), "value");

    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::filesystem::path(path.string() + ".bak"), ec);
    config.Clear();
}

} // namespace
