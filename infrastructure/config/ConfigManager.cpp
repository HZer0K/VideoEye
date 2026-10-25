#include "infrastructure/config/ConfigManager.h"
#include "infrastructure/logging/Logger.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h 会把 min/max 定义成宏，之后 std::max 之类全被撕碎
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace videoeye {

namespace {

// 保存用的临时文件名必须**唯一**（带线程号 + 单调序号）。
// 所有并发保存共用一个 <target>.tmp 时，两个线程会同时打开、互相截断对方正在写
// 的文件，最后换上去的内容可能是半截的 —— 而且这种损坏只在并发下出现，很难复现。
std::filesystem::path UniqueTempPath(const std::filesystem::path& target) {
    static std::atomic<std::uint64_t> sequence{0};
    std::ostringstream suffix;
    suffix << "." << std::this_thread::get_id() << "." << sequence.fetch_add(1, std::memory_order_relaxed) << ".tmp";
    return std::filesystem::path(target.string() + suffix.str());
}

// 原子替换：要么新文件完整在位，要么旧文件原封不动。
//
// 以前这里是"先把目标改名成 .bak、再把 .tmp rename 上去"两步走，中间存在一个
// 目标文件**不存在**的窗口（此刻进程被杀/另一个线程来读，看到的就是"没这个文件"），
// 而且替换失败后没人把 .bak 恢复回去。换成一次原子替换之后，两个问题同时消失。
//
// 平台差异必须处理:
//   * POSIX 的 rename() 覆盖同名文件，本身就是原子的；
//   * Windows 的 rename（std::filesystem::rename 走 MoveFile）**不能**覆盖已存在的
//     目标，必须显式要 MOVEFILE_REPLACE_EXISTING。
bool AtomicReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
#ifdef _WIN32
    // Windows 上"替换"要求此刻没有别的句柄开着目标文件：另一个线程正在读配置、
    // 或者杀毒软件刚扫到它，都会让这一次替换以 ACCESS_DENIED / SHARING_VIOLATION
    // 收场。这类占用是短暂的，退避重试几次比直接判"保存失败"更接近事实
    // （POSIX 的 rename 没有这个问题：它只解除目录项，开着旧文件的读者不受影响）。
    constexpr int kMaxAttempts = 10;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        ec.clear();
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return true;
        }
        const DWORD err = GetLastError();
        const bool transient = (err == ERROR_ACCESS_DENIED) || (err == ERROR_SHARING_VIOLATION) ||
                               (err == ERROR_LOCK_VIOLATION) || (err == ERROR_USER_MAPPED_FILE);
        if (!transient || attempt + 1 == kMaxAttempts) {
            ec = std::error_code(static_cast<int>(err), std::system_category());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
#else
    std::filesystem::rename(from, to, ec);
    return !ec;
#endif
}

} // namespace

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
        parsed.erase(version_it); // 版本键是元数据，不进业务配置（GetAllKeys 不该看到它）
    }
    if (version > kConfigSchemaVersion) {
        // 比本程序更新的 schema 读不懂，硬读只会得到半套配置。
        LOG_WARN("配置文件 schema 版本 (" + std::to_string(version) + ") 高于本程序 (" +
                 std::to_string(kConfigSchemaVersion) + ")，保留现有配置: " + filename);
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
    // 先在快照锁内序列化出快照 —— 文件 IO 不持这把锁，避免一次磁盘写把所有读路径堵住。
    std::string payload;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        payload = Serialize();
    }

    // 磁盘这一半由保存锁串行化：并发保存共用 .tmp / 互相 rename 是写坏配置的直接原因。
    std::lock_guard<std::mutex> save_lock(save_mutex_);

    // 原子落盘：先写临时文件，再一次替换到位。
    // 直接往目标文件写的话，写一半崩了/断电留下的就是半截配置（解析不出任何键，
    // 启动后整套默认值静默生效）。
    const std::filesystem::path target(filename);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            LOG_ERROR("无法创建配置目录: " + target.parent_path().string() + " - " + ec.message());
            return false;
        }
    }

    const std::filesystem::path tmp = UniqueTempPath(target);
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
            file.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    // 替换前留一份备份（**复制**而不是改名 —— 改名会制造"目标文件不存在"的窗口）。
    // 备份失败不影响本次保存，只记一条警告。
    std::error_code backup_ec;
    if (std::filesystem::exists(target, backup_ec)) {
        std::filesystem::copy_file(target, target.string() + ".bak", std::filesystem::copy_options::overwrite_existing,
                                   backup_ec);
        if (backup_ec) {
            LOG_WARN("配置备份失败（不影响本次保存）: " + target.string() + ".bak - " + backup_ec.message());
        }
    }

    if (!AtomicReplaceFile(tmp, target, ec)) {
        // 替换失败 = 目标文件根本没被动过，不需要"恢复备份"这一步；
        // 唯一要收拾的是自己那个临时文件。
        LOG_ERROR("配置落盘失败（原文件未改动）: " + filename + " - " + ec.message());
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
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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
