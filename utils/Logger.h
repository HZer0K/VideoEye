#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <memory>

namespace videoeye {
namespace utils {

// 日志级别枚举
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR_LEVEL = 3,
    FATAL = 4
};

// 日志类 - 线程安全的单例模式
class Logger {
public:
    // 获取单例实例
    static Logger& GetInstance();
    
    // 禁止拷贝和赋值
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    // 设置日志级别
    void SetLevel(LogLevel level);
    LogLevel GetLevel() const { return current_level_; }
    
    // 日志输出方法
    void Debug(const std::string& message, const std::string& module = "");
    void Info(const std::string& message, const std::string& module = "");
    void Warning(const std::string& message, const std::string& module = "");
    void Error(const std::string& message, const std::string& module = "");
    void Fatal(const std::string& message, const std::string& module = "");
    
    // 格式化日志
    void Log(LogLevel level, const std::string& message, const std::string& module = "");
    
    // 设置日志文件
    bool SetLogFile(const std::string& filename);
    
    // 获取日志级别字符串
    static std::string LevelToString(LogLevel level);
    
private:
    Logger();
    ~Logger();
    
    // 写入日志
    void WriteLog(const std::string& formatted_message);
    
    // 获取当前时间字符串
    std::string GetCurrentTime() const;
    
    // 成员变量
    LogLevel current_level_;
    std::mutex log_mutex_;
    std::unique_ptr<std::ofstream> log_file_;
    bool output_to_console_;
};

// 便捷宏定义
#define LOG_DEBUG(msg)  videoeye::utils::Logger::GetInstance().Debug(msg, __FUNCTION__)
#define LOG_INFO(msg)   videoeye::utils::Logger::GetInstance().Info(msg, __FUNCTION__)
#define LOG_WARN(msg)   videoeye::utils::Logger::GetInstance().Warning(msg, __FUNCTION__)
#define LOG_ERROR(msg)  videoeye::utils::Logger::GetInstance().Error(msg, __FUNCTION__)
#define LOG_FATAL(msg)  videoeye::utils::Logger::GetInstance().Fatal(msg, __FUNCTION__)

} // namespace utils
} // namespace videoeye
