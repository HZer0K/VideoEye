#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace videoeye {

// 配置文件里"schema 版本"键的名字。它是元数据而非业务配置：加载时读完即从
// map 里摘掉，GetAllKeys() 看不到它。
inline constexpr const char* kSchemaVersionKey = "config.schema_version";
// 当前 schema 版本。0 = 老版本文件（没有版本键），按旧格式兼容读取。
inline constexpr int kConfigSchemaVersion = 1;
inline constexpr int kLegacySchemaVersion = 0;

// 配置管理器 - 线程安全的单例模式
//
// 全局可变单例本身是个薄弱点（谁都能改、测试之间互相污染），这里保留现状但收紧
// 几条最容易出事的边界：
//   * 加载失败/读到更高版本时**保持现有配置**，不再用空 map 覆盖；
//   * 保存走"唯一临时文件 + 平台原子替换"，不留半截文件，也不留目标文件缺失的窗口；
//   * 快照锁（config_mutex_）只护内存里的 map，磁盘那一半由**保存锁**串行化 ——
//     文件 IO 不持快照锁（先在锁内取快照），但也不能两个线程同时写同一个 .tmp。
class ConfigManager {
public:
    // 获取单例实例
    static ConfigManager& GetInstance();

    // 禁止拷贝和赋值
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // 加载配置文件
    bool LoadFromFile(const std::string& filename);

    // 保存到文件
    bool SaveToFile(const std::string& filename);

    // 获取配置值
    std::string GetString(const std::string& key, const std::string& default_value = "") const;
    int GetInt(const std::string& key, int default_value = 0) const;
    double GetDouble(const std::string& key, double default_value = 0.0) const;
    bool GetBool(const std::string& key, bool default_value = false) const;

    // 设置配置值
    void SetString(const std::string& key, const std::string& value);
    void SetInt(const std::string& key, int value);
    void SetDouble(const std::string& key, double value);
    void SetBool(const std::string& key, bool value);

    // 检查键是否存在
    bool HasKey(const std::string& key) const;

    // 删除键
    void RemoveKey(const std::string& key);

    // 清空所有配置
    void Clear();

    // 获取所有键
    std::vector<std::string> GetAllKeys() const;

private:
    ConfigManager() = default;
    ~ConfigManager() = default;

    // 解析配置文件
    std::map<std::string, std::string> ParseFile(const std::string& filename);

    // 序列化配置
    std::string Serialize() const;

    // 成员变量
    std::map<std::string, std::string> config_map_;
    // 快照锁：护 config_map_，也护 Serialize() 读到的那份快照。
    mutable std::mutex config_mutex_;
    // 保存锁：护"写磁盘"这半段（临时文件 → 原子替换）。
    //
    // 快照锁刻意不覆盖磁盘 IO（一次慢盘写不该堵住所有读路径），但把 IO 挪到锁外
    // 之后，同一文件的两次并发保存会共用同一个 .tmp、互相截断，还会互相 rename
    // 对方刚写好的文件 —— 实测 6 线程 / 600 次保存失败 332 次。保存是低频操作，
    // 串行化的代价远小于写坏一份配置。
    std::mutex save_mutex_;
};

// 便捷宏定义
#define CONFIG videoeye::ConfigManager::GetInstance()

} // namespace videoeye
