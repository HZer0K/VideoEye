#pragma once

#include <cstddef>
#include <string_view>

namespace videoeye::model {

// FFmpeg 的图片解复用器白名单。播放侧（StreamInfoExtractor → PlayerPanel）与 QC 侧
// （container 规则）共用同一份判定，漏一个名字的后果不对称：
//   * 播放侧漏 → 播到 EOF 走普通视频收尾，画面被 ResetVideoUI 清掉（JPEG 曾如此）；
//   * QC 侧漏 → 缺音频 / 时长异常 / 不可拖动三条规则全部命中，正常图片被判失败。
// 因此新增图片格式时只改这里，别在调用点再写一份清单。
//
// 只登记本仓库自带 FFmpeg（third_party/prebuilt）里**既有解复用器又有解码器**的格式：
// 例如 .ico 有 ico 解复用器但构建里没有 ico 解码器，登记进来只会得到黑屏。
inline bool IsImageDemuxer(std::string_view name) {
    // image2: 单文件 JPEG（.jpg/.jpeg 在 FFmpeg 8.x 首选它，而不是 jpeg_pipe）、
    //         .tga / .pam，以及图片序列（frame_%04d.png）—— 都没有"播完清屏"的语义。
    // webp_anim: 新版 FFmpeg 把动图 WebP 拆成独立解复用器（静态仍走 webp_pipe）。
    return name == "gif" || name == "apng" || name == "image2" || name == "webp_anim" || name == "png_pipe" ||
           name == "jpeg_pipe" || name == "bmp_pipe" || name == "webp_pipe" || name == "tiff_pipe" ||
           name == "j2k_pipe" || name == "jpegxl_pipe" || name == "qoi_pipe" || name == "exr_pipe" ||
           name == "ppm_pipe" || name == "pgm_pipe" || name == "pbm_pipe";
}

// 图片被封装在通用容器里时（AVIF 落在 mov/mp4 家族），解复用器名无法区分
// "一段视频"与"一张图"，只能靠扩展名。这里只登记实测可解码的静图后缀。
// 大小写不敏感：调用方未归一（如单测直接传 ".AVIF"）也不能漏判。
inline bool IsStillImageExtension(std::string_view extension) {
    if (!extension.empty() && extension.front() == '.') {
        extension.remove_prefix(1);
    }
    constexpr std::string_view kAvif = "avif";
    if (extension.size() != kAvif.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kAvif.size(); ++i) {
        char c = extension[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kAvif[i]) {
            return false;
        }
    }
    return true;
}

// 播放/QC 共用的"这是图片输入"判定：解复用器命中图片白名单，或落在静图扩展名。
// extension 允许带前导点，大小写由调用方归一（本项目统一小写）。
inline bool IsImageInput(std::string_view demuxer, std::string_view extension) {
    return IsImageDemuxer(demuxer) || IsStillImageExtension(extension);
}

} // namespace videoeye::model
