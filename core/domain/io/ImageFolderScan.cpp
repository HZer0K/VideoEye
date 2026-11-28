#include "core/domain/io/ImageFolderScan.h"

#include "core/domain/io/Utf8Path.h"
#include "core/domain/model/ImageDemuxer.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace videoeye {
namespace io {

namespace {

bool IsAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// 文件名里的扩展名（含前导点，可能为空）。
std::string ExtensionOfFileName(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    // ".gitignore" 这类"点开头"的名字：点在最前面时不算扩展名。
    if (dot == std::string::npos || dot == 0) {
        return std::string();
    }
    return name.substr(dot);
}

bool IsImageFileName(const std::string& name) {
    return model::IsImageFileExtension(ExtensionOfFileName(name));
}

// 去前导零但保留一位（"007" -> "7"，"000" -> "0"）。
std::string_view TrimLeadingZeros(std::string_view digits) {
    while (digits.size() > 1 && digits.front() == '0') {
        digits.remove_prefix(1);
    }
    return digits;
}

struct NamedEntry {
    std::string name;      // 文件名（UTF-8，仅用于排序与匹配）
    std::string full_path; // 绝对路径（UTF-8）
};

void SortByName(std::vector<NamedEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const NamedEntry& lhs, const NamedEntry& rhs) { return NaturalFileNameLess(lhs.name, rhs.name); });
}

// 目录直接子项里的图片。ec 由调用方观察（读不了目录时返回空）。
std::vector<NamedEntry> EnumerateImages(const std::filesystem::path& dir) {
    std::vector<NamedEntry> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return out;
    }
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code type_ec;
        // 只收普通文件：符号链接指向目录时 is_regular_file 为 false，正是想要的
        // （不递归、不为一个坏链接中断整轮扫描）。
        if (!it->is_regular_file(type_ec) || type_ec) {
            continue;
        }
        const std::string name = ToUtf8String(it->path().filename());
        if (name.empty() || !IsImageFileName(name)) {
            continue;
        }
        out.push_back(NamedEntry{name, ToUtf8String(it->path())});
    }
    SortByName(out);
    return out;
}

} // namespace

bool NaturalFileNameLess(const std::string& lhs, const std::string& rhs) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < lhs.size() && j < rhs.size()) {
        if (IsAsciiDigit(lhs[i]) && IsAsciiDigit(rhs[j])) {
            const std::size_t lhs_start = i;
            const std::size_t rhs_start = j;
            while (i < lhs.size() && IsAsciiDigit(lhs[i])) {
                ++i;
            }
            while (j < rhs.size() && IsAsciiDigit(rhs[j])) {
                ++j;
            }
            const std::string_view lhs_digits =
                TrimLeadingZeros(std::string_view(lhs).substr(lhs_start, i - lhs_start));
            const std::string_view rhs_digits =
                TrimLeadingZeros(std::string_view(rhs).substr(rhs_start, j - rhs_start));
            // 无前导零之后，位数多的数值更大；位数相同再逐位比较。
            if (lhs_digits.size() != rhs_digits.size()) {
                return lhs_digits.size() < rhs_digits.size();
            }
            if (lhs_digits != rhs_digits) {
                return lhs_digits < rhs_digits;
            }
            continue; // 数值相等（"07" 与 "7"）：继续比后面的字符
        }
        const char lhs_char = ToLowerAscii(lhs[i]);
        const char rhs_char = ToLowerAscii(rhs[j]);
        if (lhs_char != rhs_char) {
            return lhs_char < rhs_char;
        }
        ++i;
        ++j;
    }
    // 一方是另一方的前缀：短的在前（"a.png" < "a_1.png"）。
    return (lhs.size() - i) < (rhs.size() - j);
}

ImageFolderListing ScanImageFolder(const std::string& utf8_image_path) {
    ImageFolderListing listing;
    if (utf8_image_path.empty()) {
        return listing;
    }
    const std::filesystem::path file = ToNativePath(utf8_image_path);
    if (file.empty()) {
        return listing;
    }
    const std::string current_name = ToUtf8String(file.filename());
    if (current_name.empty() || !IsImageFileName(current_name)) {
        // 非图片（视频/音频/裸流）不做兄弟翻页：把它们混进"上一张/下一张"里，
        // 用户点一下就会从看图跳到播视频，语义完全变了。
        return listing;
    }

    std::filesystem::path dir = file.parent_path();
    std::error_code cwd_ec;
    if (dir.empty()) {
        dir = std::filesystem::current_path(cwd_ec);
        if (cwd_ec) {
            return listing;
        }
    }

    std::vector<NamedEntry> entries = EnumerateImages(dir);
    if (entries.empty()) {
        return listing;
    }
    listing.files.reserve(entries.size());
    bool matched = false;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        listing.files.push_back(entries[i].full_path);
        // 文件名按 ASCII 大小写不敏感匹配：Windows 上同一文件在路径里的大小写
        // 可能与目录项不同（用户手输 / 别的程序回填），不该因此判成"不在列表里"。
        if (!matched && model::EqualsIgnoreCaseAscii(entries[i].name, current_name)) {
            listing.index = i;
            matched = true;
        }
    }
    listing.contains_current = matched;
    if (!matched) {
        listing.index = 0;
    }
    return listing;
}

std::vector<std::string> ListImagesInFolder(const std::string& utf8_folder) {
    std::vector<std::string> out;
    if (utf8_folder.empty()) {
        return out;
    }
    const std::filesystem::path dir = ToNativePath(utf8_folder);
    if (dir.empty()) {
        return out;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return out;
    }
    std::vector<NamedEntry> entries = EnumerateImages(dir);
    out.reserve(entries.size());
    for (NamedEntry& entry : entries) {
        out.push_back(std::move(entry.full_path));
    }
    return out;
}

} // namespace io
} // namespace videoeye
