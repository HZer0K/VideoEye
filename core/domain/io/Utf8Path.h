#pragma once

// 路径编码约定：core 层内部一律按 UTF-8 传 std::string，只在这里转成平台原生路径。
//
// 为什么必须转：Qt 的 QString::toStdString() 是 UTF-8，FFmpeg 返回的路径是 UTF-8，
// 清单文件与命令行参数也都是 UTF-8。而 std::ifstream/std::ofstream 在 Windows 上把
// char* 路径按**当前 ANSI 代码页**解释 —— 于是"文件能不能打开"取决于系统区域设置：
// 开发机上是英文代码页就一切正常，用户机上是 GBK/其它代码页时中文目录直接失败，
// 表现为"我这儿好好的、用户那儿一直报打不开"。
//
// std::filesystem::u8path() 把 UTF-8 显式转成 wchar_t 原生路径（Linux/macOS 上本来就是
// 按字节处理，是恒等变换），走它就与区域设置无关了。
//
// 为什么要 try/catch：u8path 遇到**非法 UTF-8** 会抛 filesystem_error（它派生自
// system_error），而本项目的 IO 接口全是"返回 bool / status"的风格，一个坏路径不该把
// 调用方炸掉。转换失败时先退回原样构造（等于改动前的行为）；若原样构造也失败（例如
// Windows 上非法字节无法映射到当前 ANSI 代码页），则返回空路径，由后续 open/stat 走
// 正常的"路径不可用"失败分支，仍然不抛异常。

#include <filesystem>
#include <string>
#include <system_error>

namespace videoeye {

inline std::filesystem::path ToNativePath(const std::string& utf8_path) {
    if (utf8_path.empty())
        return std::filesystem::path();
    // 捕获 system_error 而不是只捕 filesystem_error：MSVC 的回退构造失败时抛的是
    // std::system_error 本身，filesystem_error 派生自它，一次覆盖两种来源。
    try {
        return std::filesystem::u8path(utf8_path);
    } catch (const std::system_error&) {
        try {
            return std::filesystem::path(utf8_path);
        } catch (const std::system_error&) {
            return std::filesystem::path();
        }
    }
}

} // namespace videoeye
