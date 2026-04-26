#include "Logger.h"
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace videoeye {
namespace utils {

Logger::Logger()
    : current_level_(LogLevel::DEBUG)
    , output_to_console_(true) {
}

Logger::~Logger() {
    if (log_file_ && log_file_->is_open()) {
        log_file_->close();
    }
}

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

void Logger::SetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    current_level_ = level;
}

void Logger::Debug(const std::string& message, const std::string& module) {
    Log(LogLevel::DEBUG, message, module);
}

void Logger::Info(const std::string& message, const std::string& module) {
    Log(LogLevel::INFO, message, module);
}

void Logger::Warning(const std::string& message, const std::string& module) {
    Log(LogLevel::WARNING, message, module);
}

void Logger::Error(const std::string& message, const std::string& module) {
    Log(LogLevel::ERROR_LEVEL, message, module);
}

void Logger::Fatal(const std::string& message, const std::string& module) {
    Log(LogLevel::FATAL, message, module);
}

void Logger::Log(LogLevel level, const std::string& message, const std::string& module) {
    if (level < current_level_) {
        return;
    }
    
    // 格式化日志消息
    std::ostringstream oss;
    oss << "[" << GetCurrentTime() << "] "
        << "[" << LevelToString(level) << "] ";
    
    if (!module.empty()) {
        oss << "[" << module << "] ";
    }
    
    oss << message;
    
    std::string formatted_message = oss.str();
    WriteLog(formatted_message);
}

bool Logger::SetLogFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    log_file_ = std::make_unique<std::ofstream>(filename, std::ios::app);
    if (!log_file_->is_open()) {
        log_file_.reset();
        return false;
    }
    
    return true;
}

std::string Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:     return "DEBUG";
        case LogLevel::INFO:      return "INFO";
        case LogLevel::WARNING:   return "WARN";
        case LogLevel::ERROR_LEVEL: return "ERROR";
        case LogLevel::FATAL:     return "FATAL";
        default:                  return "UNKNOWN";
    }
}

void Logger::WriteLog(const std::string& formatted_message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    // 输出到控制台
    if (output_to_console_) {
        if (formatted_message.find("ERROR") != std::string::npos ||
            formatted_message.find("FATAL") != std::string::npos) {
            std::cerr << formatted_message << std::endl;
        } else {
            std::cout << formatted_message << std::endl;
        }
    }
    
    // 输出到文件
    if (log_file_ && log_file_->is_open()) {
        *log_file_ << formatted_message << std::endl;
        log_file_->flush();
    }
}

std::string Logger::GetCurrentTime() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::tm tm_buf;
    localtime_r(&time_t, &tm_buf);
    
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    
    return oss.str();
}

} // namespace utils
} // namespace videoeye
