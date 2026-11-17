#include "core/analysis/quality/VisualDefectAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/domain/task/ThreadBody.h"
#include "infrastructure/logging/Logger.h"

namespace videoeye {

namespace {

constexpr double kEpsilon = 1e-9;

double Clamp01(double v) {
    if (!(v == v))
        return 0.0; // NaN
    if (v < 0.0)
        return 0.0;
    if (v > 1.0)
        return 1.0;
    return v;
}

double NoValue() {
    return std::numeric_limits<double>::quiet_NaN();
}

bool Valid(double v) {
    return std::isfinite(v);
}

// 单张灰度图上的块效应分数：块边界（x%bs==0）平均落差 vs 块内部（x%bs>=2）
// 平均落差，归一化到 [0,1]。内部统计跳过紧邻边界的一列/一行，否则会把边界
// 落差算进"内部"。stride 是行跨度（可以大于 w，便于直接统计有效画面子矩形）。
// 返回 NaN 表示统计样本不足（尺寸/块宽不成立），调用方跳过。
double ComputeBlockinessScore(const uint8_t* gray, int stride, int w, int h, int bs) {
    if (!gray || stride < w || w < 16 || h < 16 || bs < 2)
        return NoValue();
    long double boundary = 0.0;
    long double interior = 0.0;
    long long bc = 0;
    long long ic = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = gray + static_cast<size_t>(y) * stride;
        for (int x = 1; x < w; ++x) {
            const int d = std::abs(static_cast<int>(row[x]) - static_cast<int>(row[x - 1]));
            const int mod = x % bs;
            if (mod == 0) {
                boundary += d;
                ++bc;
            } else if (mod >= 2) {
                interior += d;
                ++ic;
            }
        }
    }
    for (int y = 1; y < h; ++y) {
        const uint8_t* r0 = gray + static_cast<size_t>(y - 1) * stride;
        const uint8_t* r1 = r0 + stride;
        const int mod = y % bs;
        for (int x = 0; x < w; ++x) {
            const int d = std::abs(static_cast<int>(r1[x]) - static_cast<int>(r0[x]));
            if (mod == 0) {
                boundary += d;
                ++bc;
            } else if (mod >= 2) {
                interior += d;
                ++ic;
            }
        }
    }
    if (bc <= 0 || ic <= 0)
        return NoValue();
    const double b = static_cast<double>(boundary / bc);
    const double i = static_cast<double>(interior / ic);
    const double denom = b + i;
    return (denom > 1e-6) ? Clamp01((b - i) / denom) : 0.0;
}

// 分数方向: true = 越小越严重（模糊、帧差），false = 越大越严重
bool IsReverseScore(model::VisualDefectType type) {
    return type == model::VisualDefectType::Blur || type == model::VisualDefectType::FreezeFrame;
}

double MinSecondsFor(model::VisualDefectType type, const VisualDefectOptions& o) {
    switch (type) {
    case model::VisualDefectType::BlackFrame:
        return o.black_min_seconds;
    case model::VisualDefectType::FreezeFrame:
        return o.freeze_min_seconds;
    case model::VisualDefectType::Blur:
        return o.blur_min_seconds;
    case model::VisualDefectType::Flicker:
        return o.flicker_min_seconds;
    case model::VisualDefectType::Blockiness:
        return o.blockiness_min_seconds;
    case model::VisualDefectType::OverExposure:
    case model::VisualDefectType::UnderExposure:
        return o.exposure_min_seconds;
    case model::VisualDefectType::ColorCast:
        return o.color_cast_min_seconds;
    case model::VisualDefectType::InterlaceCombing:
        return o.combing_min_seconds;
    case model::VisualDefectType::Letterbox:
    case model::VisualDefectType::Pillarbox:
        return o.border_min_seconds;
    }
    return 0.3;
}

// 缺陷描述（含触发数值，方便事后复核"为什么算缺陷"）
std::string DescribeDefect(model::VisualDefectType type, double duration, double score, double aux, double threshold) {
    char buf[320];
    switch (type) {
    case model::VisualDefectType::BlackFrame:
        std::snprintf(buf, sizeof(buf), "黑场 %.2fs：黑像素占比 %.1f%%（阈值 %.0f%%），亮度均值 %.1f", duration,
                      score * 100.0, threshold * 100.0, aux);
        break;
    case model::VisualDefectType::FreezeFrame:
        std::snprintf(buf, sizeof(buf), "冻结 %.2fs：帧间差异 %.4f（阈值 %.4f），期间音频非静音", duration, score,
                      threshold);
        break;
    case model::VisualDefectType::Blur:
        std::snprintf(buf, sizeof(buf), "模糊 %.2fs：锐度指数 %.3f（阈值 %.3f，越小越糊）", duration, score, threshold);
        break;
    case model::VisualDefectType::Flicker:
        std::snprintf(buf, sizeof(buf), "闪烁 %.2fs：亮度均值最大跳变 %.1f（阈值 %.1f）", duration, score, threshold);
        break;
    case model::VisualDefectType::Blockiness:
        std::snprintf(buf, sizeof(buf), "花屏/马赛克 %.2fs：块效应强度 %.2f（阈值 %.2f）", duration, score, threshold);
        break;
    case model::VisualDefectType::OverExposure:
        std::snprintf(buf, sizeof(buf), "过曝 %.2fs：高光占比 %.1f%%，削波占比 %.1f%%", duration, score * 100.0,
                      aux * 100.0);
        break;
    case model::VisualDefectType::UnderExposure:
        std::snprintf(buf, sizeof(buf), "欠曝 %.2fs：暗部占比 %.1f%%，亮度均值 %.1f", duration, score * 100.0, aux);
        break;
    case model::VisualDefectType::ColorCast:
        std::snprintf(buf, sizeof(buf), "色偏 %.2fs：偏移强度 %.3f（阈值 %.3f），R-B = %+.1f", duration, score,
                      threshold, aux);
        break;
    case model::VisualDefectType::InterlaceCombing:
        std::snprintf(buf, sizeof(buf), "隔行梳齿 %.2fs：梳齿强度 %.2f（阈值 %.2f）", duration, score, threshold);
        break;
    case model::VisualDefectType::Letterbox:
    case model::VisualDefectType::Pillarbox: {
        // score = 两侧黑边占比之和，aux = 起始侧（上/左）占比
        const double first = Valid(aux) ? aux : score;
        const double second = std::max(0.0, score - first);
        if (type == model::VisualDefectType::Letterbox) {
            std::snprintf(buf, sizeof(buf), "上下黑边：合计 %.1f%%（上 %.1f%% / 下 %.1f%%），有效画面只剩 %.1f%%",
                          score * 100.0, first * 100.0, second * 100.0, (1.0 - score) * 100.0);
        } else {
            std::snprintf(buf, sizeof(buf), "左右黑边：合计 %.1f%%（左 %.1f%% / 右 %.1f%%），有效画面只剩 %.1f%%",
                          score * 100.0, first * 100.0, second * 100.0, (1.0 - score) * 100.0);
        }
        break;
    }
    default:
        std::snprintf(buf, sizeof(buf), "缺陷 %.2fs", duration);
        break;
    }
    return std::string(buf);
}

} // namespace

// ===========================================================================
// 单帧指标（纯算法，不碰任何内部状态）
// ===========================================================================

model::FrameQualityMetric VisualDefectAnalyzer::ComputeFrameMetrics(const model::FrameSample& sample,
                                                                    const model::FrameSample* previous,
                                                                    double previous_luma_mean,
                                                                    const VisualDefectOptions& options) {

    model::FrameQualityMetric m;
    m.frame_index = sample.frame_index;
    m.timestamp_seconds = sample.timestamp_seconds;
    m.sample_width = sample.width;
    m.sample_height = sample.height;

    if (!sample.valid()) {
        m.error_message = "Invalid frame sample";
        return m;
    }

    const int w = sample.width;
    const int h = sample.height;
    const uint8_t* g = sample.gray.data();

    // ---- 黑边 / 有效画面区域（必须先用整帧算）----
    if (options.detect_borders && w >= 16 && h >= 16) {
        std::vector<double> row_mean(static_cast<size_t>(h), 0.0);
        std::vector<double> col_mean(static_cast<size_t>(w), 0.0);
        for (int y = 0; y < h; ++y) {
            long double s = 0.0;
            const uint8_t* row = g + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x)
                s += row[x];
            row_mean[static_cast<size_t>(y)] = static_cast<double>(s / w);
        }
        for (int x = 0; x < w; ++x) {
            long double s = 0.0;
            for (int y = 0; y < h; ++y)
                s += g[static_cast<size_t>(y) * w + x];
            col_mean[static_cast<size_t>(x)] = static_cast<double>(s / h);
        }

        int top = 0;
        while (top < h && row_mean[static_cast<size_t>(top)] <= options.border_luma)
            ++top;
        int bottom = 0;
        while (bottom < h && row_mean[static_cast<size_t>(h - 1 - bottom)] <= options.border_luma)
            ++bottom;
        int left = 0;
        while (left < w && col_mean[static_cast<size_t>(left)] <= options.border_luma)
            ++left;
        int right = 0;
        while (right < w && col_mean[static_cast<size_t>(w - 1 - right)] <= options.border_luma)
            ++right;

        // 整帧都黑（黑场帧）谈不上黑边；暗场画面被误吃一半以上时也放弃判定
        const bool whole_frame_dark = (top >= h) || (left >= w);
        if (!whole_frame_dark) {
            if (top + bottom >= h / 2) {
                top = 0;
                bottom = 0;
            }
            if (left + right >= w / 2) {
                left = 0;
                right = 0;
            }
            model::ActivePictureArea area;
            area.x = left;
            area.y = top;
            area.width = w - left - right;
            area.height = h - top - bottom;
            if (area.width > 0 && area.height > 0) {
                area.valid = true;
                // 记下坐标所在的采样尺寸与对应源尺寸: 报告/界面展示的是源分辨率坐标，
                // 换算逻辑见 ActivePictureArea::SourceX() 等。源宽未知时留 0。
                area.sample_width = w;
                area.sample_height = h;
                area.source_width = sample.source_width;
                area.source_height = sample.source_height;
                area.top_bar_ratio = static_cast<double>(top) / h;
                area.bottom_bar_ratio = static_cast<double>(bottom) / h;
                area.left_bar_ratio = static_cast<double>(left) / w;
                area.right_bar_ratio = static_cast<double>(right) / w;
                area.letterbox =
                    (top > 0 && bottom > 0 && (area.top_bar_ratio + area.bottom_bar_ratio) >= options.border_bar_ratio);
                area.pillarbox =
                    (left > 0 && right > 0 && (area.left_bar_ratio + area.right_bar_ratio) >= options.border_bar_ratio);
                area.active_ratio = static_cast<double>(area.width) * area.height / (static_cast<double>(w) * h);
                m.active_area = area;
            }
        }
    }

    // 之后的指标只在有效画面区域内统计: letterbox / pillarbox 的纯黑大块会把
    // 亮度均值、锐度方差、块效应与梳齿统计拉偏（黑边与画面的交界本身就是强边缘），
    // 让带黑边素材与全画幅素材的判定口径不一致。
    // 整帧黑（黑场帧）或暗场被误判时 active_area 无效，退回整帧统计。
    int rx = 0;
    int ry = 0;
    int rw = w;
    int rh = h;
    if (m.active_area.valid && m.active_area.width >= 16 && m.active_area.height >= 16) {
        rx = m.active_area.x;
        ry = m.active_area.y;
        rw = m.active_area.width;
        rh = m.active_area.height;
    }
    const size_t n = static_cast<size_t>(rw) * static_cast<size_t>(rh);

    // ---- 亮度统计（一次遍历顺带把各档像素占比数出来）----
    // 阈值口径: 采样图保留源帧的亮度范围（见 QualityAnalyzer::ApplyNativeLumaRange），
    // limited range 素材的合法峰值白是 235，用 full range 的 240/250 去卡会把
    // 白底/标题卡误报成过曝。这里把高光/削波阈值按 limited range 重新标定，
    // 只把超过 235 的 super-white（236+，多是编码过冲或压缩到死白）算作高光/削波。
    const bool limited_range = sample.limited_range;
    const double highlight_luma = limited_range ? 236.0 : options.highlight_luma;
    const double clip_high_luma = limited_range ? 252.0 : options.clip_high_luma;
    long double sum = 0.0;
    long double sum_sq = 0.0;
    long long cnt_black = 0;
    long long cnt_dark = 0;
    long long cnt_high = 0;
    long long cnt_clip_high = 0;
    long long cnt_clip_low = 0;
    for (int y = ry; y < ry + rh; ++y) {
        const uint8_t* row = g + static_cast<size_t>(y) * w + rx;
        for (int x = 0; x < rw; ++x) {
            const long double v = static_cast<long double>(row[x]);
            sum += v;
            sum_sq += v * v;
            if (v <= options.black_luma)
                ++cnt_black;
            if (v <= options.shadow_luma)
                ++cnt_dark;
            if (v >= highlight_luma)
                ++cnt_high;
            if (v >= clip_high_luma)
                ++cnt_clip_high;
            if (v <= options.clip_low_luma)
                ++cnt_clip_low;
        }
    }

    const long double mean = sum / static_cast<long double>(n);
    long double variance = sum_sq / static_cast<long double>(n) - mean * mean;
    if (variance < 0.0)
        variance = 0.0;

    const double inv_n = 1.0 / static_cast<double>(n);
    m.luma_mean = static_cast<double>(mean);
    m.luma_std = std::sqrt(static_cast<double>(variance));
    m.black_ratio = static_cast<double>(cnt_black) * inv_n;
    m.dark_ratio = static_cast<double>(cnt_dark) * inv_n;
    m.highlight_ratio = static_cast<double>(cnt_high) * inv_n;
    m.clip_high_ratio = static_cast<double>(cnt_clip_high) * inv_n;
    m.clip_low_ratio = static_cast<double>(cnt_clip_low) * inv_n;

    // HDR（PQ / HLG）没做色调映射时，码值到显示亮度是非线性的，阈值占比没有意义 ——
    // 不置不可用就会把整片高动态范围素材判成过曝/欠曝。曝光判定依赖这些字段的
    // Valid() 检查，置 NaN 即自动关闭，无需再改判定逻辑。
    // 已经做过近似色调映射（tone_mapped）的帧码值已换算到 SDR 口径，照常判定。
    // 黑场/模糊/块效应/黑边这类结构指标仍按码值算，不受影响。
    if (sample.hdr && !sample.tone_mapped) {
        m.highlight_ratio = NoValue();
        m.clip_high_ratio = NoValue();
        m.dark_ratio = NoValue();
        m.clip_low_ratio = NoValue();
    }

    // ---- 模糊: 拉普拉斯方差 ----
    // 清晰画面有强边缘 -> 二阶差分大且分布分散（方差大）；
    // 模糊后高频被削掉 -> 二阶差分整体趋近 0，方差塌下去。
    if (options.detect_blur && rw >= 3 && rh >= 3) {
        long double lap_sum = 0.0;
        long double lap_sq = 0.0;
        long long cnt = 0;
        for (int y = ry + 1; y < ry + rh - 1; ++y) {
            const uint8_t* row = g + static_cast<size_t>(y) * w;
            const uint8_t* up = row - w;
            const uint8_t* down = row + w;
            for (int x = rx + 1; x < rx + rw - 1; ++x) {
                const int lap = 4 * static_cast<int>(row[x]) - static_cast<int>(row[x - 1]) -
                                static_cast<int>(row[x + 1]) - static_cast<int>(up[x]) - static_cast<int>(down[x]);
                lap_sum += lap;
                lap_sq += static_cast<long double>(lap) * lap;
                ++cnt;
            }
        }
        if (cnt > 0) {
            const long double mu = lap_sum / cnt;
            long double var = lap_sq / cnt - mu * mu;
            if (var < 0.0)
                var = 0.0;
            m.blur_score = static_cast<double>(var) / 1000.0;
        }
    }

    // ---- 隔行梳齿 ----
    // 逐行相邻的差（跨场，含时间差）远大于隔行相邻（同场）时，说明素材是隔行的。
    // 归一化成 (full - field) / (full + field)：逐行素材这个值 <= 0，隔行有运动时接近 1。
    if (options.detect_combing && rh >= 4 && rw >= 2) {
        long double full = 0.0;
        long double field = 0.0;
        long long cnt = 0;
        for (int y = ry; y + 2 < ry + rh; ++y) {
            const uint8_t* r0 = g + static_cast<size_t>(y) * w + rx;
            const uint8_t* r1 = r0 + w;
            const uint8_t* r2 = r1 + w;
            for (int x = 0; x < rw; ++x) {
                full += std::abs(static_cast<int>(r0[x]) - static_cast<int>(r1[x]));
                field += std::abs(static_cast<int>(r0[x]) - static_cast<int>(r2[x]));
            }
            cnt += rw;
        }
        if (cnt > 0) {
            const double fd = static_cast<double>(full / cnt);
            const double ff = static_cast<double>(field / cnt);
            const double denom = fd + ff;
            m.combing_score = (denom > 1e-6) ? Clamp01((fd - ff) / denom) : 0.0;
        }
    }

    // ---- 花屏 / 马赛克（块效应）----
    // 采样图经过缩放，8px 编码块的周期已经被抹平（1920 源缩到 256 宽时一个块只剩
    // 1px 多），在采样图上找块边界只会把缩放痕迹当误报。因此优先用 BuildSample
    // 生成的 1:1 源分辨率局部裁剪图（block_probes），此时 bs 就是源像素的 block_size。
    // 裁剪图取了中心 + 四角多块，每块各自算分再取最大：花屏/马赛克常常只出现在
    // 画面局部（例如运动区、台标附近），只看中心会被局部损伤平均掉。
    // 没有探针（手造样本 / 探针失败）时才退回采样图，并按 源宽/样本宽 折算 bs；
    // 折算结果不足 2px 直接置 NoValue，绝不能进入 x % bs（bs 可能为 0，会除零崩溃）。
    if (options.detect_blockiness) {
        const int nominal_bs = options.block_size > 1 ? options.block_size : 8;
        double best = NoValue();
        if (sample.has_block_probes()) {
            for (const auto& probe : sample.block_probes) {
                if (!probe.valid())
                    continue;
                const double score =
                    ComputeBlockinessScore(probe.gray.data(), probe.width, probe.width, probe.height, nominal_bs);
                if (Valid(score) && (!Valid(best) || score > best))
                    best = score;
            }
        } else {
            int bs = nominal_bs;
            if (sample.source_width > w) {
                bs = static_cast<int>(nominal_bs * static_cast<double>(w) / static_cast<double>(sample.source_width));
            }
            // bs < 2 时块结构在采样图上已不可分辨 —— 置 NoValue，不误报
            if (bs >= 2)
                best = ComputeBlockinessScore(g + static_cast<size_t>(ry) * w + rx, w, rw, rh, bs);
        }
        m.blockiness_score = best;
    }

    // ---- 色偏 ----
    // HDR 的 RGB 码值经过 PQ/HLG 传递函数，未做色调映射时 R/G/B 均值差反映的是
    // 传递曲线而不是真实白平衡，置为不可用（has_rgb 仍为真，只是不判定）。
    if (options.detect_color_cast && sample.has_rgb() && (!sample.hdr || sample.tone_mapped)) {
        // RGB 小图与灰度采样图同比例缩放，按相同的相对区域换算，黑边不计入均值。
        // 取整后可能落到 0 宽/高（极小区域），此时退回整幅 RGB。
        int gx0 = 0;
        int gy0 = 0;
        int gx1 = sample.rgb_width;
        int gy1 = sample.rgb_height;
        if (rw != w || rh != h) {
            gx0 = rx * sample.rgb_width / w;
            gy0 = ry * sample.rgb_height / h;
            gx1 = (rx + rw) * sample.rgb_width / w;
            gy1 = (ry + rh) * sample.rgb_height / h;
            if (gx1 <= gx0 || gy1 <= gy0) {
                gx0 = 0;
                gy0 = 0;
                gx1 = sample.rgb_width;
                gy1 = sample.rgb_height;
            }
        }
        long double sr = 0.0;
        long double sg = 0.0;
        long double sb = 0.0;
        const uint8_t* rgb = sample.rgb.data();
        const size_t stride = static_cast<size_t>(sample.rgb_width) * 3u;
        for (int y = gy0; y < gy1; ++y) {
            const uint8_t* row = rgb + static_cast<size_t>(y) * stride + static_cast<size_t>(gx0) * 3u;
            for (int x = gx0; x < gx1; ++x) {
                sr += row[0];
                sg += row[1];
                sb += row[2];
                row += 3;
            }
        }
        const double pixels = static_cast<double>(gx1 - gx0) * (gy1 - gy0);
        const double mr = static_cast<double>(sr / pixels);
        const double mg = static_cast<double>(sg / pixels);
        const double mb = static_cast<double>(sb / pixels);
        const double my = 0.299 * mr + 0.587 * mg + 0.114 * mb;
        const double dr = mr - my;
        const double db = mb - my;
        m.color_cast_score = Clamp01(std::sqrt(dr * dr + db * db) / 255.0);
        m.color_cast_rb = mr - mb;
    }

    // ---- 时域: 与上一采样帧的差异 ----
    if (previous && previous->valid() && previous->width == w && previous->height == h) {
        long double mad = 0.0;
        const uint8_t* p = previous->gray.data();
        for (int y = ry; y < ry + rh; ++y) {
            const uint8_t* cur_row = g + static_cast<size_t>(y) * w + rx;
            const uint8_t* prev_row = p + static_cast<size_t>(y) * w + rx;
            for (int x = 0; x < rw; ++x) {
                mad += std::abs(static_cast<int>(cur_row[x]) - static_cast<int>(prev_row[x]));
            }
        }
        m.frame_diff = static_cast<double>(mad / n) / 255.0;
        if (Valid(previous_luma_mean)) {
            m.luma_delta = std::abs(m.luma_mean - previous_luma_mean);
        }
    }

    m.valid = true;
    return m;
}

// ===========================================================================
// 生命周期
// ===========================================================================

VisualDefectAnalyzer::VisualDefectAnalyzer() = default;

VisualDefectAnalyzer::~VisualDefectAnalyzer() {
    StopWorker();
}

void VisualDefectAnalyzer::Reset(const VisualDefectOptions& options) {
    StopWorker();
    std::lock_guard<std::mutex> lock(m_);
    options_ = options;
    metrics_.clear();
    defects_.clear();
    timeline_defect_begin_ = 0;
    pending_metrics_.clear();
    pending_defects_.clear();
    active_areas_.clear();
    segments_.clear();
    luma_history_.clear();
    luma_history_ts_.clear();
    has_prev_ = false;
    prev_sample_ = model::FrameSample();
    prev_luma_mean_ = NoValue();
    last_audio_active_ts_ = NoValue();
    next_defect_id_ = 1;
    samples_truncated_ = false;
    defects_truncated_ = false;
    evidence_defects_ = 0;
    realtime_processing_ = false;
    effective_area_cache_valid_ = false;
    effective_area_cache_ = model::ActivePictureArea();
    dropped_samples_.store(0);
    analyzed_samples_.store(0);
}

void VisualDefectAnalyzer::StartWorker(size_t queue_capacity) {
    StopWorker();
    {
        std::lock_guard<std::mutex> lock(m_);
        queue_capacity_ = (queue_capacity == 0) ? 1 : queue_capacity;
        queue_.clear();
    }
    worker_running_.store(true);
    // 必须在起线程**之前**清掉: 线程一起来就可能立刻跑到收尾(队列空且已经被告知停止),
    // 晚一步清会让 StopWorker 把一个已经结束的线程误判成"还没回来"。
    worker_exited_.store(false, std::memory_order_release);
    worker_ = std::thread(&VisualDefectAnalyzer::WorkerLoop, this);
}

void VisualDefectAnalyzer::StopWorker() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_);
            worker_running_.store(false);
        }
        cv_.notify_all();
        // 为什么这里是**有预算的等待 + 仍然 join**, 而不是像 QcRunner 那样超预算就放弃:
        // 那条 ledger 的关键在于任务体持不持有宿主。这里的线程跑的是成员函数
        // (std::thread(&VisualDefectAnalyzer::WorkerLoop, this)), 一旦 detach, 它会在宿主
        // 析构之后继续读写 m_/queue_/options_ —— 放弃比多等危险得多。
        // 所以预算的用处是**观测**: 把"任务体没按契约响应停止"这件事报出来, 而不是替它收尾。
        //
        // 契约上它一定回得来: WorkerLoop 每轮都看 worker_running_(而且 cv_ 已经 notify 过),
        // 单帧处理又是纯计算, 不会卡在第三方阻塞调用上。
        if (!AwaitWorkerExit(kStopBudgetMs)) {
            LOG_ERROR("画面质检工作线程未在 " + std::to_string(kStopBudgetMs) +
                      "ms 内响应停止请求, 将继续等待它退出(线程体每轮都会检查停止标志)");
        }
        worker_.join();
        worker_exited_.store(true, std::memory_order_release);
    } else {
        worker_running_.store(false);
    }
    std::lock_guard<std::mutex> lock(m_);
    queue_.clear();
}

bool VisualDefectAnalyzer::AwaitWorkerExit(int budget_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (!worker_exited_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool VisualDefectAnalyzer::Submit(const model::FrameSample& sample) {
    if (!worker_running_.load())
        return false;
    std::unique_lock<std::mutex> lock(m_);
    if (queue_.size() >= queue_capacity_) {
        // 播放压力大时宁可丢分析帧，也不能反压解码线程
        dropped_samples_.fetch_add(1);
        return false;
    }
    queue_.push_back(sample);
    lock.unlock();
    cv_.notify_one();
    return true;
}

// 收尾必须托底: 无论从哪个出口离开都要把 worker_exited_ 置位, 否则 StopWorker 会把
// 一个已经结束的线程判成"还没回来", 白白等到预算耗尽才 join。这里改由 ScopeExit
// 结构性地保证 —— 以前靠"try/catch 之后的那一行", 哪天在中间加个 return 就漏。
// 顺带把异常兜住 —— 成员函数作线程入口时 std::thread 的入口是 noexcept, 异常逃逸
// 会直接 std::terminate(), 连"分析线程为什么不见了"都无从查起。收口模板与其它
// 线程入口共用同一份(core/domain/task/ThreadBody.h)。
void VisualDefectAnalyzer::WorkerLoop() {
    const auto settle = task::MakeScopeExit([this]() { worker_exited_.store(true, std::memory_order_release); });
    const std::optional<std::string> failure = task::GuardBody([this]() {
        for (;;) {
            model::FrameSample sample;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return !queue_.empty() || !worker_running_.load(); });
                if (queue_.empty()) {
                    if (!worker_running_.load())
                        break;
                    continue;
                }
                sample = std::move(queue_.front());
                queue_.pop_front();
            }
            std::lock_guard<std::mutex> lock(m_);
            ProcessSampleLocked(sample, true);
        }
    });
    if (failure) {
        LOG_ERROR("画面质检工作线程异常终止: " + *failure);
    }
}

void VisualDefectAnalyzer::Feed(const model::FrameSample& sample) {
    std::lock_guard<std::mutex> lock(m_);
    ProcessSampleLocked(sample, false);
}

void VisualDefectAnalyzer::Flush(double end_timestamp_seconds) {
    std::lock_guard<std::mutex> lock(m_);
    CloseAllSegmentsLocked(end_timestamp_seconds, -1);
}

void VisualDefectAnalyzer::ResetTimeline(double end_timestamp_seconds) {
    const bool restart_worker = worker_running_.load();
    StopWorker();
    {
        std::lock_guard<std::mutex> lock(m_);
        CloseAllSegmentsLocked(end_timestamp_seconds, -1);
        timeline_defect_begin_ = defects_.size();
        has_prev_ = false;
        prev_sample_ = model::FrameSample();
        prev_luma_mean_ = NoValue();
        last_audio_active_ts_ = NoValue();
        luma_history_.clear();
        luma_history_ts_.clear();
    }
    if (restart_worker) {
        StartWorker(queue_capacity_);
    }
}

std::vector<model::FrameQualityMetric> VisualDefectAnalyzer::TakePendingMetrics() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<model::FrameQualityMetric> out;
    out.swap(pending_metrics_);
    return out;
}

std::vector<model::VisualDefect> VisualDefectAnalyzer::TakePendingDefects() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<model::VisualDefect> out;
    out.swap(pending_defects_);
    return out;
}

model::ActivePictureArea VisualDefectAnalyzer::EffectiveArea() const {
    std::lock_guard<std::mutex> lock(m_);
    if (!effective_area_cache_valid_) {
        effective_area_cache_ = ComputeEffectiveAreaLocked();
        effective_area_cache_valid_ = true;
    }
    return effective_area_cache_;
}

model::ActivePictureArea VisualDefectAnalyzer::ComputeEffectiveAreaLocked() const {
    std::vector<int> xs;
    std::vector<int> ys;
    std::vector<int> ws;
    std::vector<int> hs;
    std::vector<int> sws;
    std::vector<int> shs;
    std::vector<int> srcs_w;
    std::vector<int> srcs_h;
    std::vector<double> ratios;
    std::vector<double> bars_top;
    std::vector<double> bars_bottom;
    std::vector<double> bars_left;
    std::vector<double> bars_right;
    const size_t n = active_areas_.size();
    xs.reserve(n);
    ys.reserve(n);
    ws.reserve(n);
    hs.reserve(n);
    sws.reserve(n);
    shs.reserve(n);
    srcs_w.reserve(n);
    srcs_h.reserve(n);
    ratios.reserve(n);
    bars_top.reserve(n);
    bars_bottom.reserve(n);
    bars_left.reserve(n);
    bars_right.reserve(n);
    for (const auto& a : active_areas_) {
        if (!a.valid)
            continue;
        xs.push_back(a.x);
        ys.push_back(a.y);
        ws.push_back(a.width);
        hs.push_back(a.height);
        ratios.push_back(a.active_ratio);
        bars_top.push_back(a.top_bar_ratio);
        bars_bottom.push_back(a.bottom_bar_ratio);
        bars_left.push_back(a.left_bar_ratio);
        bars_right.push_back(a.right_bar_ratio);
        if (a.sample_width > 0 && a.sample_height > 0) {
            sws.push_back(a.sample_width);
            shs.push_back(a.sample_height);
        }
        if (a.source_width > 0 && a.source_height > 0) {
            srcs_w.push_back(a.source_width);
            srcs_h.push_back(a.source_height);
        }
    }
    model::ActivePictureArea area;
    if (xs.empty())
        return area;

    auto median_int = [](std::vector<int>& v) {
        if (v.empty())
            return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    auto median_double = [](std::vector<double>& v) {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    area.valid = true;
    area.x = median_int(xs);
    area.y = median_int(ys);
    area.width = median_int(ws);
    area.height = median_int(hs);
    area.active_ratio = median_double(ratios);
    // 四条边各自取中位数后再判定，不能只看 x>0/y>0 —— 采样噪声会在边缘留下 1px
    // 的暗行，单帧足以让"有没有黑边"翻车；中位数 + 占比阈值才是全片口径。
    area.top_bar_ratio = median_double(bars_top);
    area.bottom_bar_ratio = median_double(bars_bottom);
    area.left_bar_ratio = median_double(bars_left);
    area.right_bar_ratio = median_double(bars_right);
    area.letterbox = (area.top_bar_ratio + area.bottom_bar_ratio) >= options_.border_bar_ratio;
    area.pillarbox = (area.left_bar_ratio + area.right_bar_ratio) >= options_.border_bar_ratio;
    area.sample_width = median_int(sws);
    area.sample_height = median_int(shs);
    area.source_width = median_int(srcs_w);
    area.source_height = median_int(srcs_h);
    return area;
}

model::VisualDefectReport VisualDefectAnalyzer::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_);
    model::VisualDefectReport report;
    report.defects = defects_;
    report.samples = metrics_;
    if (!effective_area_cache_valid_) {
        effective_area_cache_ = ComputeEffectiveAreaLocked();
        effective_area_cache_valid_ = true;
    }
    report.effective_area = effective_area_cache_;
    report.analyzed = analyzed_samples_.load() > 0;
    report.analyzed_frames = analyzed_samples_.load();
    report.dropped_frames = dropped_samples_.load();
    report.samples_truncated = samples_truncated_;
    report.defects_truncated = defects_truncated_;
    if (!metrics_.empty()) {
        report.duration_seconds = metrics_.back().timestamp_seconds - metrics_.front().timestamp_seconds;
    }
    return report;
}

// ===========================================================================
// 逐帧处理
// ===========================================================================

void VisualDefectAnalyzer::ProcessSampleLocked(const model::FrameSample& sample, bool realtime) {
    if (!sample.valid())
        return;

    realtime_processing_ = realtime;
    model::FrameQualityMetric m =
        ComputeFrameMetrics(sample, has_prev_ ? &prev_sample_ : nullptr, prev_luma_mean_, options_);
    if (!m.valid)
        return;

    metrics_.push_back(m);
    // 离线 Feed 没有消费者取 pending_*，写进去只是把整份结果再拷一份（离线全帧时等于翻倍）。
    if (realtime)
        pending_metrics_.push_back(m);
    // 超上限时按批裁剪: 每帧 erase 头部 1 条是 O(n) 搬移，200k 条样本下离线分析会被
    // 拖成 O(n^2)。这里一次多丢一点，之后若干帧都不用再搬。
    if (metrics_.size() > options_.max_samples) {
        samples_truncated_ = true;
        const size_t keep = options_.max_samples;
        size_t batch = std::max<size_t>(keep / 8, 256);
        batch = std::min(batch, std::max<size_t>(keep / 2, 1));
        size_t drop = metrics_.size() - keep + batch;
        if (drop > metrics_.size())
            drop = metrics_.size();
        metrics_.erase(metrics_.begin(), metrics_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    if (m.active_area.valid) {
        active_areas_.push_back(m.active_area);
        effective_area_cache_valid_ = false;
        // 中位数用不着保留全部采样帧，超长素材只留最近一段，避免长视频把内存吃满
        if (active_areas_.size() > kMaxActiveAreaSamples) {
            active_areas_.erase(active_areas_.begin(),
                                active_areas_.begin() +
                                    static_cast<std::ptrdiff_t>(active_areas_.size() - kMaxActiveAreaSamples));
        }
    }
    analyzed_samples_.fetch_add(1);

    const double ts = m.timestamp_seconds;
    const int frame = m.frame_index;

    // 闪烁窗口要用到历史亮度，先更新再判定。
    // 按时间限窗（flicker_window_seconds）而不是固定点数: 1fps 与 5fps 档位的
    // 点数窗口长度相差 5 倍，会让时长/严重度口径不一致。
    luma_history_.push_back(m.luma_mean);
    luma_history_ts_.push_back(ts);
    const double flicker_window = options_.flicker_window_seconds > 0.0 ? options_.flicker_window_seconds : 3.0;
    while (luma_history_.size() > 2 && (ts - luma_history_ts_.front()) > flicker_window) {
        luma_history_.pop_front();
        luma_history_ts_.pop_front();
    }
    // 极端帧率下的兜底上限（3s 内 4096 个采样点已远超任何档位）
    while (luma_history_.size() > 4096) {
        luma_history_.pop_front();
        luma_history_ts_.pop_front();
    }

    const bool black_hit = options_.detect_black && Valid(m.black_ratio) && m.black_ratio >= options_.black_ratio &&
                           m.luma_mean <= options_.black_luma_mean;

    // 音频静音按"连续静音"判定: 说话间隙里的单帧静音不能把冻结洗白。
    // 只有静音持续 freeze_audio_silence_seconds 以上，才认为这一刻本来就没有声音。
    if (!sample.audio_silent)
        last_audio_active_ts_ = ts;
    const double silence_grace =
        options_.freeze_audio_silence_seconds > 0.0 ? options_.freeze_audio_silence_seconds : 1.0;
    const bool audio_silent =
        sample.audio_silent && (!Valid(last_audio_active_ts_) || ts - last_audio_active_ts_ >= silence_grace);

    const bool freeze_hit = options_.detect_freeze && has_prev_ && Valid(m.frame_diff) &&
                            m.frame_diff <= options_.freeze_diff && !audio_silent && !black_hit;

    // 模糊判定要排除"画面本来就没有内容"的帧: 纯色/黑场的拉普拉斯方差天然接近 0，
    // 那是"没细节"不是"糊了"，交给黑场/欠曝去报。
    const bool blur_hit = options_.detect_blur && Valid(m.blur_score) && m.blur_score <= options_.blur_threshold &&
                          m.black_ratio < 0.5 && m.luma_std >= options_.blur_min_luma_std;

    double flicker_amplitude = 0.0;
    double flicker_start = NoValue();
    const bool flicker_hit =
        options_.detect_flicker && FlickerHitLocked(ts, &flicker_amplitude, &flicker_start) && !black_hit;

    const bool over_hit = options_.detect_exposure && Valid(m.highlight_ratio) &&
                          (m.clip_high_ratio >= options_.over_exposure_clip_ratio ||
                           (m.highlight_ratio >= options_.over_exposure_highlight_ratio && m.clip_high_ratio >= 0.02));

    // HDR 的码值亮度不是显示亮度，dark_ratio 虽已置 NaN，但 luma_mean 还要给黑场/闪烁用，
    // 这里必须显式排除，否则暗部场景的 PQ 码值会被当成"欠曝"。
    // 做过近似色调映射后码值已是 SDR 口径，恢复判定。
    const bool hdr_unmapped = sample.hdr && !sample.tone_mapped;
    const bool under_hit =
        options_.detect_exposure && !hdr_unmapped && !black_hit && Valid(m.dark_ratio) &&
        (m.dark_ratio >= options_.under_exposure_dark_ratio || m.luma_mean <= options_.under_exposure_luma_mean);

    const bool cast_hit =
        options_.detect_color_cast && Valid(m.color_cast_score) && m.color_cast_score >= options_.color_cast_score;

    const bool combing_hit = options_.detect_combing && Valid(m.combing_score) &&
                             m.combing_score >= options_.combing_score &&
                             (!Valid(m.frame_diff) || m.frame_diff >= options_.combing_motion_gate);

    SegmentUpdate u;
    u.ts = ts;
    u.frame = frame;

    if (options_.detect_black) {
        u.type = model::VisualDefectType::BlackFrame;
        u.hit = black_hit;
        u.score = m.black_ratio;
        u.aux = m.luma_mean;
        u.threshold = options_.black_ratio;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_freeze) {
        u.type = model::VisualDefectType::FreezeFrame;
        u.hit = freeze_hit;
        u.score = m.frame_diff;
        u.aux = NoValue();
        u.threshold = options_.freeze_diff;
        u.reverse = true;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_blur) {
        u.type = model::VisualDefectType::Blur;
        u.hit = blur_hit;
        u.score = m.blur_score;
        u.aux = NoValue();
        u.threshold = options_.blur_threshold;
        u.reverse = true;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_flicker) {
        u.type = model::VisualDefectType::Flicker;
        u.hit = flicker_hit;
        u.score = flicker_amplitude;
        u.aux = NoValue();
        u.threshold = options_.flicker_delta;
        u.reverse = false;
        // 从第一次真正发生亮暗交替的采样点算起，而不是把整个窗口的前端都算进缺陷时长
        u.start_ts = flicker_start;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_exposure) {
        u.type = model::VisualDefectType::OverExposure;
        u.hit = over_hit;
        u.score = m.highlight_ratio;
        u.aux = m.clip_high_ratio;
        u.threshold = options_.over_exposure_highlight_ratio;
        u.reverse = false;
        u.start_ts = NoValue();
        UpdateSegmentLocked(u, sample);

        u.type = model::VisualDefectType::UnderExposure;
        u.hit = under_hit;
        u.score = m.dark_ratio;
        u.aux = m.luma_mean;
        u.threshold = options_.under_exposure_dark_ratio;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_color_cast) {
        u.type = model::VisualDefectType::ColorCast;
        u.hit = cast_hit;
        u.score = m.color_cast_score;
        u.aux = m.color_cast_rb;
        u.threshold = options_.color_cast_score;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_combing) {
        u.type = model::VisualDefectType::InterlaceCombing;
        u.hit = combing_hit;
        u.score = m.combing_score;
        u.aux = NoValue();
        u.threshold = options_.combing_score;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);
    }
    // 块效应：detect_blockiness 开关下按 blockiness_score 阈值命中并更新分段。
    if (options_.detect_blockiness) {
        u.type = model::VisualDefectType::Blockiness;
        u.hit = Valid(m.blockiness_score) && m.blockiness_score >= options_.blockiness_score;
        u.score = m.blockiness_score;
        u.aux = NoValue();
        u.threshold = options_.blockiness_score;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_borders && m.active_area.valid) {
        u.type = model::VisualDefectType::Letterbox;
        u.hit = m.active_area.letterbox;
        u.score = m.active_area.top_bar_ratio + m.active_area.bottom_bar_ratio;
        u.aux = m.active_area.top_bar_ratio;
        u.threshold = options_.border_bar_ratio;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);

        u.type = model::VisualDefectType::Pillarbox;
        u.hit = m.active_area.pillarbox;
        u.score = m.active_area.left_bar_ratio + m.active_area.right_bar_ratio;
        u.aux = m.active_area.left_bar_ratio;
        u.threshold = options_.border_bar_ratio;
        UpdateSegmentLocked(u, sample);
    }

    // 时域状态推进: 只保留帧差需要的字段。整份拷贝会把块效应探针（最多 5 块、
    // 约 320KB）和 RGB 证据图也带进来，而上一帧只用得到降采样灰度与尺寸，
    // 实时分析长时间运行时这是纯浪费（每帧多留几百 KB）。
    prev_sample_ = model::FrameSample();
    prev_sample_.frame_index = sample.frame_index;
    prev_sample_.timestamp_seconds = sample.timestamp_seconds;
    prev_sample_.width = sample.width;
    prev_sample_.height = sample.height;
    prev_sample_.gray = sample.gray;
    prev_luma_mean_ = m.luma_mean;
    has_prev_ = true;
}

void VisualDefectAnalyzer::UpdateSegmentLocked(const SegmentUpdate& u, const model::FrameSample& sample) {
    SegmentState& st = segments_[u.type];
    if (u.hit) {
        if (!st.active) {
            st.active = true;
            st.start_ts = Valid(u.start_ts) ? u.start_ts : u.ts;
            st.start_frame = u.frame;
            st.worst = u.score;
            st.sum = u.score;
            st.count = 1;
            st.aux = u.aux;
            st.threshold = u.threshold;
            if (sample.has_rgb()) {
                st.has_evidence = true;
                st.evidence.width = sample.rgb_width;
                st.evidence.height = sample.rgb_height;
                st.evidence.rgb = sample.rgb;
            } else {
                st.has_evidence = false;
                st.evidence = model::EvidenceFrame();
            }
        } else {
            st.worst = u.reverse ? std::min(st.worst, u.score) : std::max(st.worst, u.score);
            st.sum += u.score;
            ++st.count;
            // 辅助数值取"更极端"的那个方向：曝光取更大，色偏取绝对值更大
            if (Valid(u.aux) && Valid(st.aux)) {
                st.aux = (std::abs(u.aux) > std::abs(st.aux)) ? u.aux : st.aux;
            } else if (Valid(u.aux)) {
                st.aux = u.aux;
            }
        }
        st.last_ts = u.ts;
        st.last_frame = u.frame;
    } else if (st.active) {
        CloseSegmentLocked(u.type, u.ts, u.frame);
    }
}

void VisualDefectAnalyzer::CloseSegmentLocked(model::VisualDefectType type, double end_ts, int end_frame) {
    auto it = segments_.find(type);
    if (it == segments_.end() || !it->second.active)
        return;

    const SegmentState st = it->second;
    it->second = SegmentState();

    // 收尾时间取"最后一个命中帧"与调用方给的时间中更晚的那个:
    // Flush(素材结束时间) 通常晚于最后一个采样点，否则素材在结束前就恢复正常的
    // 那一段会被少算（缺陷时长偏短）。
    double end = st.last_ts;
    if (Valid(end_ts) && end_ts > end)
        end = end_ts;
    const double duration = std::max(0.0, end - st.start_ts);
    const int end_frame_index = (end_frame >= 0) ? end_frame : st.last_frame;
    if (duration + kEpsilon < MinSecondsFor(type, options_))
        return;

    // 与上一条同类缺陷挨得太近就合并，避免同一段问题被切成好几条
    if (defects_.size() > timeline_defect_begin_) {
        model::VisualDefect& last = defects_.back();
        if (last.type == type && st.start_ts >= last.end_seconds &&
            (st.start_ts - last.end_seconds) <= options_.MergeGapSeconds()) {
            last.end_seconds = end;
            last.end_frame = end_frame_index;
            const bool reverse = IsReverseScore(type);
            last.score = reverse ? std::min(last.score, st.worst) : std::max(last.score, st.worst);
            const double total = std::max(0.0, last.end_seconds - last.start_seconds);
            const auto merged = SeverityFor(type, total, last.score, options_);
            if (static_cast<int>(merged) > static_cast<int>(last.severity))
                last.severity = merged;
            last.description = DescribeDefect(type, total, last.score, st.aux, st.threshold);
            return;
        }
    }

    model::VisualDefect defect;
    defect.id = next_defect_id_++;
    defect.type = type;
    defect.severity = SeverityFor(type, duration, st.worst, options_);
    defect.start_seconds = st.start_ts;
    defect.end_seconds = end;
    defect.start_frame = st.start_frame;
    defect.end_frame = end_frame_index;
    defect.score = st.worst;
    defect.threshold = st.threshold;
    defect.description = DescribeDefect(type, duration, st.worst, st.aux, st.threshold);
    if (st.has_evidence)
        defect.evidence = st.evidence;
    PushDefectLocked(std::move(defect));
}

void VisualDefectAnalyzer::CloseAllSegmentsLocked(double end_ts, int end_frame) {
    // 先拷贝一份 key，CloseSegmentLocked 会改 segments_
    std::vector<model::VisualDefectType> types;
    types.reserve(segments_.size());
    for (const auto& kv : segments_) {
        if (kv.second.active)
            types.push_back(kv.first);
    }
    for (const auto& type : types) {
        CloseSegmentLocked(type, end_ts, end_frame);
    }
}

void VisualDefectAnalyzer::PushDefectLocked(model::VisualDefect defect) {
    if (defects_.size() >= options_.max_defects) {
        // 以前超上限直接静默丢弃，报告里看不出"结果被截断"
        defects_truncated_ = true;
        return;
    }
    // 证据缩略图单独设配额: 每条 27KB 左右，离线全帧跑长片能产出上千条，
    // 全留既吃内存也没人看（报告只内嵌少量）。超配额的缺陷保留文字、丢掉图。
    if (defect.evidence.valid()) {
        if (evidence_defects_ >= options_.max_evidence_defects) {
            defect.evidence = model::EvidenceFrame();
        } else {
            ++evidence_defects_;
        }
    }
    defects_.push_back(std::move(defect));
    if (realtime_processing_)
        pending_defects_.push_back(defects_.back());
}

bool VisualDefectAnalyzer::FlickerHitLocked(double ts, double* amplitude, double* window_start) const {
    if (luma_history_.size() < 4)
        return false;

    const double window = options_.flicker_window_seconds > 0.0 ? options_.flicker_window_seconds : 3.0;
    // 用窗口内的最大跳变作为强度: 取均值会被"只有少数几次真跳变"的窗口稀释，
    // 明明闪得很厉害却卡在阈值之下。
    double max_delta = 0.0;
    int alternations = 0;
    double first_alternation_ts = model::kQualityNoValue;
    for (size_t i = 1; i < luma_history_.size(); ++i) {
        const double d = luma_history_[i] - luma_history_[i - 1];
        max_delta = std::max(max_delta, std::abs(d));
        if (i < 2)
            continue;
        const double prev_d = luma_history_[i - 1] - luma_history_[i - 2];
        // 亮->暗->亮 才算闪烁；单向缓慢变化（淡入淡出）不算
        const bool opposite = d * prev_d < 0.0;
        if (!opposite || std::abs(d) < options_.flicker_delta || std::abs(prev_d) < options_.flicker_delta)
            continue;
        if (ts - luma_history_ts_[i] > window)
            continue;
        ++alternations;
        if (!Valid(first_alternation_ts))
            first_alternation_ts = luma_history_ts_[i - 2];
    }
    if (amplitude)
        *amplitude = max_delta;
    if (window_start)
        *window_start = first_alternation_ts;
    return alternations >= options_.flicker_min_alternations && max_delta >= options_.flicker_delta;
}

model::VisualDefectSeverity VisualDefectAnalyzer::SeverityFor(model::VisualDefectType type, double duration_seconds,
                                                              double score, const VisualDefectOptions& options) {
    using model::VisualDefectSeverity;
    using model::VisualDefectType;
    switch (type) {
    case VisualDefectType::BlackFrame:
        if (duration_seconds >= 3.0)
            return VisualDefectSeverity::Error;
        if (duration_seconds >= 1.0)
            return VisualDefectSeverity::Warning;
        return VisualDefectSeverity::Info;
    case VisualDefectType::FreezeFrame:
        if (duration_seconds >= 3.0)
            return VisualDefectSeverity::Error;
        return VisualDefectSeverity::Warning;
    case VisualDefectType::Blur:
        // 越低于阈值越离谱
        if (score <= options.blur_threshold * 0.25)
            return VisualDefectSeverity::Warning;
        return VisualDefectSeverity::Info;
    case VisualDefectType::Flicker:
        if (score >= options.flicker_delta * 3.0)
            return VisualDefectSeverity::Error;
        return VisualDefectSeverity::Warning;
    case VisualDefectType::Blockiness:
        if (score >= 0.6)
            return VisualDefectSeverity::Error;
        return VisualDefectSeverity::Warning;
    case VisualDefectType::OverExposure:
    case VisualDefectType::UnderExposure:
        if (score >= 0.9)
            return VisualDefectSeverity::Error;
        return VisualDefectSeverity::Warning;
    case VisualDefectType::ColorCast:
        if (score >= options.color_cast_score * 2.0)
            return VisualDefectSeverity::Warning;
        return VisualDefectSeverity::Info;
    case VisualDefectType::InterlaceCombing:
        if (score >= 0.6)
            return VisualDefectSeverity::Error;
        return VisualDefectSeverity::Warning;
    case VisualDefectType::Letterbox:
    case VisualDefectType::Pillarbox:
        // 有黑边本身不是错，只是要让人知道有效画面区域在哪
        return VisualDefectSeverity::Info;
    }
    return VisualDefectSeverity::Info;
}

} // namespace videoeye
