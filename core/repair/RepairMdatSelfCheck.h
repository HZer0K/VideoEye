#pragma once

// 重建产物的 mdat 结构自检（无参考 moov 重建 / 索引重建共用）。
//
// 历史事故：两个重建器的 WriteMdatHeader 把 size 写进了下标 0..3，却把
// 下标 4..7 一直留成 0 —— 'mdat' 四字符被写在了下标 0..3 然后立刻覆盖掉，
// 于是产物里有一个「长度为 N、类型全零」的盒。FFmpeg 能靠 stco 偏移读回
// 样本，函数也照常 ok=true，但产物不是合法 ISOBMFF。
// 「能被解析器读回 moov / 轨道数」证明不了数据盒的类型与载荷覆盖，所以这里
// 单独钉死：mdat 存在、类型确为 mdat、载荷长度与写入字节数一致、样本偏移
// 落在载荷区间内。

#include <cstdint>
#include <string>

#include "core/media/container/IsobmffParser.h"

namespace videoeye {
namespace repair {

// 生成 mdat 盒头，返回实际长度（8 / 16）。
// 布局：[0..3] size / [4..7] type='mdat' / [8..15] largesize（仅扩展分支）。
// 两个重建器共用一份实现，单测可直接覆盖普通头与 >4 GiB 的扩展头分支。
inline std::size_t MakeMdatHeader(std::uint64_t payload_size, std::uint8_t header[16]) {
    for (int i = 0; i < 16; ++i)
        header[i] = 0;
    header[4] = 'm';
    header[5] = 'd';
    header[6] = 'a';
    header[7] = 't';
    const bool large = payload_size + 8 > 0xFFFFFFFFull;
    const std::size_t header_size = large ? 16 : 8;
    const std::uint64_t size = payload_size + header_size;
    if (large) {
        header[3] = 1;
        for (int i = 0; i < 8; ++i)
            header[8 + i] = static_cast<std::uint8_t>(size >> (56 - 8 * i));
    } else {
        header[0] = static_cast<std::uint8_t>(size >> 24);
        header[1] = static_cast<std::uint8_t>(size >> 16);
        header[2] = static_cast<std::uint8_t>(size >> 8);
        header[3] = static_cast<std::uint8_t>(size);
    }
    return header_size;
}

// first_sample_offset / last_sample_end：重建样本在产物文件里的绝对字节区间
// （无样本时都传 0）。expected_payload_bytes：实际写入 mdat 的载荷字节数。
inline bool VerifyRebuiltMdat(const IsobmffFile& file, std::uint64_t first_sample_offset, std::uint64_t last_sample_end,
                              std::uint64_t expected_payload_bytes, std::string* error) {
    const IsobmffBox* mdat = nullptr;
    int mdat_count = 0;
    for (const auto& box : file.top_level) {
        if (box.type != "mdat")
            continue;
        ++mdat_count;
        if (mdat == nullptr)
            mdat = &box;
    }
    if (mdat_count != 1 || mdat == nullptr || !file.found_mdat) {
        *error = "重建结果结构自检失败：缺少类型正确的 mdat 数据盒（找到 " + std::to_string(mdat_count) + " 个）";
        return false;
    }
    if (mdat->header_size != 8 && mdat->header_size != 16) {
        *error = "重建结果结构自检失败：mdat 盒头长度异常";
        return false;
    }
    if (mdat->size < mdat->header_size || mdat->offset + mdat->size > file.file_size) {
        *error = "重建结果结构自检失败：mdat 盒越界";
        return false;
    }
    const std::uint64_t payload_start = mdat->offset + mdat->header_size;
    const std::uint64_t payload_end = mdat->offset + mdat->size;
    if (payload_end - payload_start != expected_payload_bytes) {
        *error = "重建结果结构自检失败：mdat 载荷长度 " + std::to_string(payload_end - payload_start) + " 与实际写入 " +
                 std::to_string(expected_payload_bytes) + " 字节不一致";
        return false;
    }
    if (last_sample_end > first_sample_offset &&
        (first_sample_offset < payload_start || last_sample_end > payload_end)) {
        *error = "重建结果结构自检失败：样本区间 [" + std::to_string(first_sample_offset) + ", " +
                 std::to_string(last_sample_end) + ") 未落在 mdat 载荷 [" + std::to_string(payload_start) + ", " +
                 std::to_string(payload_end) + ") 内";
        return false;
    }
    return true;
}

} // namespace repair
} // namespace videoeye
