#include "infrastructure/config/ConfigManager.h"
#include "infrastructure/logging/Logger.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace videoeye {

ConfigManager& ConfigManager::GetInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::LoadFromFile(const std::string& filename) {
    // 解析不持锁：磁盘 IO 不该堵住配置读路径。解析结果先落进局部 map，
    // 确认可用之后才一次性换进去。
    std::map<std::string, std::string> parsed = ParseFile(filename);
    if (parsed.empty()) {
        // 读不到 / 读空 = 这次加载失败。原来这里会把空 map 直接写进 config_map_，
        // 于是"文件被临时锁住 / 磁盘抖动 / 路径写错"会连带把内存里已经配好的
        // 配置一起清空 —— 失败必须保持现状。
        LOG_WARN("配置文件为空或不存在，保留现有配置: " + filename);
        return false;
    }

    int version = kLegacySchemaVersion;
    const auto version_it = parsed.find(kSchemaVersionKey);
    if (version_it != parsed.end()) {
        try {
            version = std::stoi(version_it->second);
        } catch (const std::exception&) {
            version = kLegacySchemaVersion;
        }
        parsed.erase(version_it);  // 版本键是元数据，不进业务配置（GetAllKeys 不该看到它）
    }
    if (version > kConfigSchemaVersion) {
        // 比本程序更新的 schema 读不懂，硬读只会得到半套配置。
        LOG_WARN("配置文件 schema 版本 (" + std::to_string(version) + ") 高于本程序 ("
                 + std::to_string(kConfigSchemaVersion) + ")，保留现有配置: " + filename);
        return false;
    }

    std::size_t loaded = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_map_ = std::move(parsed);
        loaded = config_map_.size();
    }

    LOG_INFO("已加载配置文件: " + filename + " (" + std::to_string(loaded) + " 项)");
    return true;
}

bool ConfigManager::SaveToFile(const std::string& filename) {
    // 先在锁内序列化出快照 —— 文件 IO 挪到锁外，避免一次磁盘写把所有读路径堵住。
    std::string payload;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        payload = Serialize();
    }

    // 原子落盘：先写 .tmp，再把 .tmp 换成目标文件。
    // 直接往目标文件写的话，写一半崩了/断电留下的就是半截配置（解析不出任何键，
    // 启动后整套默认值静默生效）。换文件的最后一步是 rename，要么新配置完整在位、
    // 要么旧配置原封不动。
    const std::filesystem::path target(filename);
    const std::filesystem::path tmp = target.string() + ".tmp";
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            LOG_ERROR("无法创建配置临时文件: " + tmp.string());
            return false;
        }
        file << payload;
        file.flush();
        if (!file) {
            LOG_ERROR("写入配置临时文件失败: " + tmp.string());
            return false;
        }
    }
    // Windows 的 rename 不能覆盖已存在的文件：把旧文件先改名成 .bak（留一条退路），
    // 再让 .tmp 就位。这两步之间若进程被杀，最多是配置回到默认值，不会是半个文件。
    std::filesystem::remove(target.string() + ".bak", ec);
    std::filesystem::rename(target, target.string() + ".bak", ec);
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        LOG_ERROR("配置落盘失败（原文件已保留为 .bak）: " + filename + " - " + ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }

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
    
    // 转换为小写。::tolower 的参数必须是"能表示成 unsigned char 的值"，
    // 直接把 char 传进去时，中文/带重音字符的高位是 1 -> 负值的 char，
    // 属于未定义行为。这里先转 unsigned char 再转回来。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    
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
    // schema 版本写在正文里（而不是只出现在注释里）：将来字段改名/换语义时，
    // 加载方能据此拒绝读不懂的高版本文件，而不是静默拿半套配置继续跑。
    oss << kSchemaVersionKey << " = " << kConfigSchemaVersion << std::endl;
    oss << std::endl;
    
    for (const auto& pair : config_map_) {
        oss << pair.first << " = " << pair.second << std::endl;
    }
    
    return oss.str();
}

} // namespace videoeye
