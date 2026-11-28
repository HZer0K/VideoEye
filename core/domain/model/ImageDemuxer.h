#pragma once

#include <array>
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
// 这条约定不是注释说说而已：tests/unit/test_image_demuxer_whitelist.cpp 会遍历本表，
// 用 av_find_input_format + avcodec_find_decoder 逐个断言齐备 —— 换 FFmpeg 构建
// （比如裁掉 jxl/exr/qoi 解码器）会在这里直接判红，而不是等用户看到黑屏。
//
// image2: 单文件 JPEG（.jpg/.jpeg 在 FFmpeg 8.x 首选它，而不是 jpeg_pipe）、
//         .tga / .pam，以及图片序列（frame_%04d.png）—— 都没有"播完清屏"的语义。
// webp_anim: 新版 FFmpeg 把动图 WebP 拆成独立解复用器（静态仍走 webp_pipe）。
//
// 唯一的前瞻登记：webp_anim 在本仓库自带的 FFmpeg 62 构建里**不存在**
// （av_find_input_format("webp_anim") == nullptr）。留着它不是为了本构建，而是因为
// "白名单多一个不存在的名字"没有任何副作用，而"将来构建里有、白名单里没有"的代价
// 是画面播完被清掉（JPEG 踩过一次）。白名单测试用 kAbsentByDesign 把这个例外
// 显式记账：它消失/出现都要求同步更新台账，不会悄悄漂移。
// 本构建的动图 WebP 走 webp_pipe 但解不出画面，由
// OpenController::IsUndecodableAnimatedWebp 在打开期明确失败。
inline constexpr std::array<std::string_view, 16> kImageDemuxers = {
    "gif",       "apng",     "image2",      "webp_anim", "png_pipe", "jpeg_pipe", "bmp_pipe", "webp_pipe",
    "tiff_pipe", "j2k_pipe", "jpegxl_pipe", "qoi_pipe",  "exr_pipe", "ppm_pipe",  "pgm_pipe", "pbm_pipe",
};

// 有真实时间轴的动图解复用器：它们的帧率是真值（gif 按帧延时算出），
// 而静图的 avg_frame_rate 是解复用器填的占位值（实测 jpg/png 恒 25）。
// 两者在"要不要显示帧率"上必须分开，否则要么给静图编造 25 fps，要么把动图帧率也抹掉。
inline constexpr std::array<std::string_view, 3> kAnimatedImageDemuxers = {"gif", "apng", "webp_anim"};

// 图片被封装在通用容器里时（AVIF / HEIC / HEIF 都落在 mov 家族），解复用器名无法
// 区分"一段视频"与"一张图"，只能靠扩展名。这里只登记实测可解码的静图后缀：
// HEIC/HEIF 走 mov 家族 + hevc 解码器，本构建实测能解出一帧（见 ImageOrientation
// 与打开流程的回归记录）；HEIF 序列（msf1，一般是 .heics）不列，播成多帧视频没有意义。
// 大小写不敏感：调用方未归一（如单测直接传 ".AVIF"）也不能漏判。
inline constexpr std::array<std::string_view, 3> kStillImageExtensions = {"avif", "heic", "heif"};

// 可打开的图片**文件后缀**（含上面那三个容器家族的静图）。
//
// 与 kImageDemuxers 的分工：那张表回答"解复用器算不算图片"，这张表回答"目录里哪些
// 文件算同一族图片"。图片翻页与图片序列靠它枚举兄弟文件，所以它必须覆盖「打开对话框
// 里能选中的所有图片后缀」—— 少一个的后果是序列里凭空缺几帧，而 FFmpeg 明明能解。
// 它不决定能不能播（那看解复用器），只决定"要不要把它列进这组图片"。
//
// 一致性由 tests/unit/test_image_demuxer_whitelist.cpp 守着：每个后缀都必须出现在
// FormatDetector 的扩展名表里（否则打开对话框看不见它，序列里却会列出来）。
// .ico 不列：有解复用器但没有 ico 解码器，登记进来只会得到黑屏（与 kImageDemuxers 同约定）。
inline constexpr std::array<std::string_view, 24> kImageFileExtensions = {
    "apng", "avif", "bmp", "exr", "gif", "heic", "heif", "j2c", "j2k", "jp2", "jpeg", "jpg",
    "jxl",  "pam",  "pbm", "pgm", "png", "pnm",  "ppm",  "qoi", "tga", "tif", "tiff", "webp",
};

// ASCII 大小写不敏感比较（不引入 locale，不分配）。
inline bool EqualsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        char c = lhs[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != rhs[i]) {
            return false;
        }
    }
    return true;
}

inline bool IsImageDemuxer(std::string_view name) {
    for (const std::string_view candidate : kImageDemuxers) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

// 动图（有时间轴、帧率有意义）：GIF / APNG / 动图 WebP。
inline bool IsAnimatedImageDemuxer(std::string_view name) {
    for (const std::string_view candidate : kAnimatedImageDemuxers) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

inline bool IsStillImageExtension(std::string_view extension) {
    if (!extension.empty() && extension.front() == '.') {
        extension.remove_prefix(1);
    }
    for (const std::string_view candidate : kStillImageExtensions) {
        if (EqualsIgnoreCaseAscii(extension, candidate)) {
            return true;
        }
    }
    return false;
}

// 文件后缀是不是图片（kImageFileExtensions，含容器家族静图）。允许带前导点。
inline bool IsImageFileExtension(std::string_view extension) {
    if (!extension.empty() && extension.front() == '.') {
        extension.remove_prefix(1);
    }
    for (const std::string_view candidate : kImageFileExtensions) {
        if (EqualsIgnoreCaseAscii(extension, candidate)) {
            return true;
        }
    }
    return false;
}

// 播放/QC 共用的"这是图片输入"判定：解复用器命中图片白名单，或落在静图扩展名。
// extension 允许带前导点，大小写由调用方归一（本项目统一小写）。
inline bool IsImageInput(std::string_view demuxer, std::string_view extension) {
    return IsImageDemuxer(demuxer) || IsStillImageExtension(extension);
}

// 图片输入是否应当显示帧率。
//
// 静图的 avg_frame_rate 是解复用器填的占位值（实测 jpg/png 恒 25/1），照抄会在
// 首页摘要与媒体信息里写成"静图 @ 25 fps"，把占位值当成事实；动图（gif/apng）的
// 帧率按帧延时算出，是真值，必须保留。非图片输入一律保留（普通视频行为不变）。
inline bool ShouldReportFrameRate(bool image_input, std::string_view demuxer) {
    return !image_input || IsAnimatedImageDemuxer(demuxer);
}

} // namespace videoeye::model
