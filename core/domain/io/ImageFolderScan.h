#pragma once

// 同一目录下的「同族图片」枚举 —— 图片翻页（上一张/下一张）与图片序列的共同底座。
//
// 为什么需要它：打开一张图后，"这张图是哪一组里的第几张""下一张是什么"是纯文件系统
// 问题，但它同时被两处用（翻页、序列播放），而且两处对"什么叫同一族"必须给出**同一个**
// 答案 —— 翻页能翻到 frame_0100.png，序列里就该有它。所以判定与排序都落在这里，
// UI 只消费结果。
//
// 分层：core/domain/io 是叶子（只依赖 stdlib + 同层的 Utf8Path / ImageDemuxer）。
//
// ⚠️ 路径一律按 UTF-8 std::string 进出（见 Utf8Path.h）：QString::toStdString() 是
//    UTF-8，若这里直接收 std::filesystem::path 而不走 ToNativePath，Windows 中文目录
//    会被按 ANSI 代码页解释，表现为"翻页翻不到、序列是空的"。

#include <cstddef>
#include <string>
#include <vector>

namespace videoeye {
namespace io {

// 一次「当前图片在它所在目录里的位置」查询的结果。
struct ImageFolderListing {
    // 自然排序后的同族图片绝对路径（UTF-8，含当前文件本身）。
    std::vector<std::string> files;
    // 当前文件在 files 里的下标；contains_current 为 false 时无意义（保持 0）。
    std::size_t index = 0;
    // 当前文件本身是否在列表里。false 表示"它不是可识别的图片"（扩展名不在
    // kImageFileExtensions 里 / 路径无效 / 目录读不了）—— 此时上一张/下一张必须禁用，
    // 而不是拿一个看似合理的下标去翻到别的文件。
    bool contains_current = false;

    bool has_previous() const {
        return contains_current && index > 0;
    }
    bool has_next() const {
        return contains_current && index + 1 < files.size();
    }
};

// 扫描 image_path 所在目录下与它同族的图片（只看直接子项，不递归）。
//
// 排序用自然序（frame_2.png 排在 frame_10.png 前面）：抽帧输出几乎全是这种命名，
// 逐字节比较会把 10 排到 2 前面，"下一张"直接跳到第 10 帧。
ImageFolderListing ScanImageFolder(const std::string& utf8_image_path);

// 目录下（不递归）的全部图片绝对路径，自然排序。供"图片序列"使用。
// 目录不存在 / 不是目录 / 无读权限时返回空（不抛异常）。
std::vector<std::string> ListImagesInFolder(const std::string& utf8_folder);

// 自然序比较两个**文件名**（不是完整路径：路径里的盘符/目录名不该参与数值比较）。
// 公开出来只为让"排序口径"能被单测直接钉住。
bool NaturalFileNameLess(const std::string& lhs, const std::string& rhs);

} // namespace io
} // namespace videoeye
