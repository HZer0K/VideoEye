#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <memory>

namespace videoeye {
namespace utils {

// 配置管理器 - 线程安全的单例模式
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
    mutable std::mutex config_mutex_;
};

// 便捷宏定义
#define CONFIG videoeye::utils::ConfigManager::GetInstance()

} // namespace utils
} // namespace videoeye
