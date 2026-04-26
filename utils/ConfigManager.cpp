#include "ConfigManager.h"
#include "Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace videoeye {
namespace utils {

ConfigManager& ConfigManager::GetInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::LoadFromFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    config_map_ = ParseFile(filename);
    
    if (config_map_.empty()) {
        LOG_WARN("配置文件为空或不存在: " + filename);
        return false;
    }
    
    LOG_INFO("已加载配置文件: " + filename + " (" + 
             std::to_string(config_map_.size()) + " 项)");
    return true;
}

bool ConfigManager::SaveToFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建配置文件: " + filename);
        return false;
    }
    
    file << Serialize();
    file.close();
    
    LOG_INFO("已保存配置文件: " + filename);
    return true;
}

std::string ConfigManager::GetString(const std::string& key, const std::string& default_value) const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        return it->second;
    }
    return default_value;
}

int ConfigManager::GetInt(const std::string& key, int default_value) const {
    std::string value = GetString(key);
    if (value.empty()) {
        return default_value;
    }
    
    try {
        return std::stoi(value);
    } catch (const std::exception& e) {
        LOG_WARN("配置值不是有效的整数: " + key + " = " + value);
        return default_value;
    }
}

double ConfigManager::GetDouble(const std::string& key, double default_value) const {
    std::string value = GetString(key);
    if (value.empty()) {
        return default_value;
    }
    
    try {
        return std::stod(value);
    } catch (const std::exception& e) {
        LOG_WARN("配置值不是有效的浮点数: " + key + " = " + value);
        return default_value;
    }
}

bool ConfigManager::GetBool(const std::string& key, bool default_value) const {
    std::string value = GetString(key);
    if (value.empty()) {
        return default_value;
    }
    
    // 转换为小写
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    } else if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    
    LOG_WARN("配置值不是有效的布尔值: " + key + " = " + value);
    return default_value;
}

void ConfigManager::SetString(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_map_[key] = value;
}

void ConfigManager::SetInt(const std::string& key, int value) {
    SetString(key, std::to_string(value));
}

void ConfigManager::SetDouble(const std::string& key, double value) {
    SetString(key, std::to_string(value));
}

void ConfigManager::SetBool(const std::string& key, bool value) {
    SetString(key, value ? "true" : "false");
}

bool ConfigManager::HasKey(const std::string& key) const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_map_.find(key) != config_map_.end();
}

void ConfigManager::RemoveKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_map_.erase(key);
}

void ConfigManager::Clear() {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_map_.clear();
}

std::vector<std::string> ConfigManager::GetAllKeys() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    std::vector<std::string> keys;
    for (const auto& pair : config_map_) {
        keys.push_back(pair.first);
    }
    return keys;
}

std::map<std::string, std::string> ConfigManager::ParseFile(const std::string& filename) {
    std::map<std::string, std::string> config;
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        return config;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // 去除首尾空白
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        // 跳过空行和注释
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // 查找等号
        size_t pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        
        // 提取键和值
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        
        // 去除键的空白
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        
        // 去除值的空白和引号
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        
        if (!key.empty()) {
            config[key] = value;
        }
    }
    
    return config;
}

std::string ConfigManager::Serialize() const {
    std::ostringstream oss;
    
    oss << "# VideoEye Configuration File" << std::endl;
    oss << "# Generated automatically" << std::endl;
    oss << std::endl;
    
    for (const auto& pair : config_map_) {
        oss << pair.first << " = " << pair.second << std::endl;
    }
    
    return oss.str();
}

} // namespace utils
} // namespace videoeye
