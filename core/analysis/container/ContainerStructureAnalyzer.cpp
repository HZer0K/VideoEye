#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "core/analysis/detail/AnalysisTextUtil.h"
#include "infrastructure/concurrency/Cancellation.h"
#include "core/ffmpeg_io/FfmpegInterrupt.h"  // 共享 FFmpeg 中断回调
#include "core/analysis/orchestration/FormatDetector.h"
#include "core/analysis/container/Mp4BoxAnalyzer.h"
#include "core/analysis/container/Mp4SampleTableAnalyzer.h"
#include "core/analysis/container/EbmlAnalyzer.h"
#include "core/analysis/container/AviStructureAnalyzer.h"
#include "core/analysis/container/FlvStructureAnalyzer.h"
#include "core/analysis/container/TsStructureAnalyzer.h"
#include "core/analysis/container/AsfStructureAnalyzer.h"
#include "core/analysis/container/OggStructureAnalyzer.h"
#include "core/analysis/streaming/HlsManifestAnalyzer.h"
#include "core/analysis/streaming/DashManifestAnalyzer.h"
#include "core/analysis/streaming/SegmentQcAnalyzer.h"
#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/ScopedTimer.h"
#include <cstddef>
#include <string>
#include <vector>

#include <functional>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace videoeye {

namespace {
// domain 侧早就不用 std::string 了，这两个小工具只在本文件内部用：
// 把 std::vector<std::string>::join() 与定宽小数格式化换成 std 版本。
//
// 注意 trimmed 这一路已经不在这里了：QString::trimmed() 的替身统一收在
// detail/AnalysisTextUtil.h 的 TrimCopy()，全 analysis 层只此一份。本文件原先另有个
// 同名 Trimmed 并与它行为不一致（只认空格/tab/CR/LF，不认 \f \v），属于"第二份实现"，
// 现在调用点一律走 TrimCopy。
std::string Join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}
std::string Format(const char* fmt, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return std::string(buf);
}

// 收尾成"已取消"：valid 必须清掉（调用方可能已经把 valid 置过 true 又在后面
// 被取消），否则界面会把一份被中断的分析当成成功结果签收。
bool MarkCancelled(model::ContainerStructureResult& result) {
    result.valid = false;
    if (result.error_message.empty()) result.error_message = "已取消";
    return false;
}

// MP4 stsd 采样描述 fourcc → 可读编码名。
// 覆盖 AV1 (av01) / VP9 (vp09) 及常见 H.264/HEVC/AAC 等，
// 未匹配的 fourcc 返回空串，调用方回退显示原始 fourcc。
std::string Mp4FourccToName(const std::string& fourcc) {
    if (fourcc == "av01") return "AV1";
    if (fourcc == "vp08") return "VP8";
    if (fourcc == "vp09") return "VP9";
    if (fourcc == "avc1" || fourcc == "avc2" ||
        fourcc == "avc3" || fourcc == "avc4") return "H.264 (AVC)";
    if (fourcc == "hev1" || fourcc == "hvc1") return "H.265 (HEVC)";
    if (fourcc == "vvc1" || fourcc == "vvi1") return "H.266 (VVC)";
    if (fourcc == "dva1") return "Dolby Vision (AV1 base)";
    if (fourcc == "dvav") return "Dolby Vision (AVC base)";
    if (fourcc == "dvhe" || fourcc == "dvh1") return "Dolby Vision (HEVC base)";
    if (fourcc == "mp4v") return "MPEG-4 Visual";
    if (fourcc == "mp4a") return "MPEG-4 Audio (AAC)";
    if (fourcc == "opus") return "Opus";
    if (fourcc == "fLaC") return "FLAC";
    if (fourcc == "alac") return "ALAC";
    if (fourcc == "ac-3") return "AC-3";
    if (fourcc == "ec-3") return "E-AC-3";
    if (fourcc == "ac-4") return "AC-4";
    if (fourcc == "mlpa") return "TrueHD (MLP)";
    if (fourcc == "stpp") return "Timed Text (STPP)";
    return std::string();
}
} // namespace

ContainerStructureAnalyzer::ContainerStructureAnalyzer() = default;
ContainerStructureAnalyzer::~ContainerStructureAnalyzer() = default;

bool ContainerStructureAnalyzer::Analyze(const std::string& file_path,
                                          model::ContainerStructureResult& result,
                                          std::shared_ptr<std::atomic<bool>> cancel,
                                          const StageCallback& on_stage) {
    VE_PERF("ContainerStructureAnalyzer::Analyze");
    result.file_path = file_path;
    LOG_INFO("ContainerStructureAnalyzer::Analyze ENTER: " + file_path);

    // 已经被取消（如关闭流程触发 CancelAll）就别再启动重型解析，避免关闭挂死
    if (cancel && cancel->load(std::memory_order_acquire)) {
        return MarkCancelled(result);
    }

    // 1. 检测格式
    auto fmt = FormatDetector::Detect(file_path);
    result.format = fmt;
    result.format_name = FormatDetector::FormatName(fmt);

    LOG_INFO("容器结构分析: 检测到格式 = " + result.format_name);
    LOG_INFO("ContainerStructureAnalyzer: 分发到对应解析器, format=" + std::to_string(static_cast<int>(fmt)));

    // 2. 根据格式分发到对应解析器
    switch (fmt) {
    case model::ContainerFormat::MP4:
    case model::ContainerFormat::MOV: {
        Mp4BoxAnalyzer mp4_analyzer;
        bool mp4_ok = false;
        {
            VE_PERF("Mp4BoxAnalyzer::AnalyzeFile(box 树)");
            mp4_ok = mp4_analyzer.AnalyzeFile(file_path, result.mp4_detail, cancel.get());
        }
        if (!mp4_ok) {
            // 专用解析器挂了才回退 FFmpeg；被中断不算"失败"。
            if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
            LOG_WARN("MP4 专用解析器失败, 回退到 FFmpeg 通用分析");
            return AnalyzeWithFFmpeg(file_path, result, cancel);
        }

        // 每个阶段之间都补一次取消检查：辅助函数各自会查，但它们返回 false 只说明
        // "这一次没走完"，到底是取消还是解析失败得由这里判定 —— 只有取消才禁止回退 FFmpeg。
        {
            if (on_stage) on_stage(Stage::kBuildTree);
            auto t0 = std::chrono::steady_clock::now();
            if (!ConvertMp4Tree(result.mp4_detail.box_tree, 0, result.element_tree, cancel.get())) {
                return MarkCancelled(result);
            }
            auto t1 = std::chrono::steady_clock::now();
            LOG_INFO("ContainerStructureAnalyzer: ConvertMp4Tree 耗时 = " +
                     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()) + " ms");
            if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
            if (on_stage) on_stage(Stage::kExtractStreamInfo);

            if (!ExtractMp4StreamInfo(result.mp4_detail.box_tree, result, cancel.get())) {
                return MarkCancelled(result);
            }
            auto t2 = std::chrono::steady_clock::now();
            LOG_INFO("ContainerStructureAnalyzer: ExtractMp4StreamInfo 耗时 = " +
                     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()) + " ms");
            if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);

            // valid 只在所有阶段都跑完之后才置：中间任何一步被取消都要能把它清掉。
            result.valid = true;

            // 样本级一致性校验（stbl 全表交叉校验 / elst / moof-traf-trun / faststart）。
            // 与 Mp4BoxAnalyzer 是两次独立解析：那边走 Inspector 字符串（为了展示 box 树），
            // 这边直接读原子拿数值（为了算偏移与时间）。失败不影响结构树展示。
            Mp4SampleTableAnalyzer sample_analyzer;
            bool sample_ok = false;
            {
                VE_PERF("Mp4SampleTableAnalyzer::AnalyzeFile(容器页)");
                sample_ok = sample_analyzer.AnalyzeFile(file_path,
                                                        result.mp4_samples,
                                                        Mp4SampleTableOptions{},
                                                        cancel.get());
            }
            // 样本表这一路是"额外"的分析：它解析失败只是少几条一致性结论，可以继续出图；
            // 但**被取消就必须整条放弃** —— 否则上面那份 valid=true 的结果会被发出去，
            // 用户看到的"分析完成"其实是被打断的半成品。
            if (!sample_ok) {
                if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
                LOG_WARN("MP4 样本表校验失败: " + result.mp4_samples.error_message);
            } else {
                LOG_INFO("ContainerStructureAnalyzer: MP4 样本表校验 issues=" +
                         std::to_string(result.mp4_samples.issues.size()));
            }
            if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
        }

        if (on_stage) on_stage(Stage::kElementCount);
        {
            int box_count = 0;
            bool count_cancelled = false;
            std::function<int(const std::vector<model::Mp4BoxNode>&)> count;
            count = [&](const std::vector<model::Mp4BoxNode>& nodes) -> int {
                if (count_cancelled) return 0;
                if (infrastructure::Checkpoint(cancel.get())) {
                    count_cancelled = true;
                    return 0;
                }
                int c = nodes.size();
                for (const auto& n : nodes) c += count(n.children);
                return c;
            };
            box_count = count(result.mp4_detail.box_tree);
            // 与 EBML 侧同一条规则: 计数递归被取消就必须整条放弃, 不能把半成品 summary
            // 配着 valid=true 发出去。
            if (count_cancelled) return MarkCancelled(result);
            result.summary = (StrCat("MP4 Box | 顶级: %1 | 总计: %2 | Track: %3", result.mp4_detail.box_tree.size(), box_count, result.streams.size()))
                                 ;
        }
        return result.valid;
    }

    case model::ContainerFormat::MKV:
    case model::ContainerFormat::WebM: {
        EbmlAnalyzer ebml_analyzer;
        if (ebml_analyzer.Analyze(file_path, result.ebml_detail, cancel.get())) {
            if (on_stage) on_stage(Stage::kBuildTree);
            if (!ConvertEbmlTree(result.ebml_detail.element_tree, 0, result.element_tree,
                                 cancel.get())) {
                return MarkCancelled(result);
            }
            if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
            if (on_stage) on_stage(Stage::kExtractStreamInfo);

            // 取消令牌必须传下去（以前漏了）：轨道列表是这一段里唯一可能长到
            // "值得中断"的循环，不传的话点取消要等整个 Tracks 段读完才生效。
            if (!ExtractEbmlStreamInfo(result.ebml_detail, result, cancel.get())) {
                return MarkCancelled(result);
            }

            if (on_stage) on_stage(Stage::kElementCount);
            int elem_count = 0;
            bool count_cancelled = false;
            std::function<int(const std::vector<model::EbmlElementNode>&)> count;
            count = [&](const std::vector<model::EbmlElementNode>& nodes) -> int {
                if (count_cancelled) return 0;
                if (infrastructure::Checkpoint(cancel.get())) {
                    count_cancelled = true;
                    return 0;
                }
                int c = nodes.size();
                for (const auto& n : nodes) c += count(n.children);
                return c;
            };
            elem_count = count(result.ebml_detail.element_tree);
            if (count_cancelled) return MarkCancelled(result);

            // valid 只在所有阶段都跑完之后才置：中间任何一步被取消都要能把它清掉
            // （MarkCancelled 负责清）。以前它在 ExtractEbmlStreamInfo 之后就置了，
            // 后面的计数递归即使被取消也已经来不及。
            result.summary = (StrCat("%1 | %2 个元素 | %3 轨道", (result.ebml_detail.doc_type), elem_count, result.streams.size()))
                                 ;
            result.valid = true;
        } else {
            // 同上：解析被中断不是"失败"，此时回退 FFmpeg 会把一次取消换成"分析成功"。
            if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
            LOG_WARN("EBML 专用解析器失败, 回退到 FFmpeg 通用分析");
            return AnalyzeWithFFmpeg(file_path, result, cancel);
        }
        return result.valid;
    }

    case model::ContainerFormat::AVI: {
        AviStructureAnalyzer avi;
        if (avi.Analyze(file_path, result, cancel.get())) return true;
        if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
        LOG_WARN("AVI 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result, cancel);
    }
    case model::ContainerFormat::FLV: {
        FlvStructureAnalyzer flv;
        if (flv.Analyze(file_path, result, cancel.get())) return true;
        if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
        LOG_WARN("FLV 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result, cancel);
    }
    case model::ContainerFormat::MPEG_TS: {
        TsStructureAnalyzer ts;
        if (ts.Analyze(file_path, result, cancel.get())) return true;
        if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
        LOG_WARN("TS 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result, cancel);
    }
    case model::ContainerFormat::HLS:
    case model::ContainerFormat::DASH: {
        // 三态：被取消时不得回退 FFmpeg，也不得把半份清单当有效结果。
        if (AnalyzeStreamingManifest(file_path, result, cancel.get()) == StageStatus::kDone) return true;
        if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
        LOG_WARN("流媒体清单解析失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result, cancel);
    }
    case model::ContainerFormat::ASF: {
        AsfStructureAnalyzer asf;
        if (asf.Analyze(file_path, result, cancel.get())) return true;
        if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
        LOG_WARN("ASF 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result, cancel);
    }
    case model::ContainerFormat::OGG: {
        OggStructureAnalyzer ogg;
        if (ogg.Analyze(file_path, result, cancel.get())) return true;
        if (infrastructure::Checkpoint(cancel.get())) return MarkCancelled(result);
        LOG_WARN("OGG 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result, cancel);
    }

    default:
        // FFmpeg 通用回退
        return AnalyzeWithFFmpeg(file_path, result, cancel);
    }
}

bool ContainerStructureAnalyzer::ExtractMp4StreamInfo(const std::vector<model::Mp4BoxNode>& box_tree,
                                                        model::ContainerStructureResult& result,
                                                        const std::atomic<bool>* cancel) {
    // 遍历 box 树, 找到所有 trak, 提取流信息
    //
    // 取消必须冒泡到**函数返回值**：以前只是 `return;` 跳回当前这层 walk，
    // 外层循环接着往下走，最后照样 return true —— 结果就是取消之后 ExtractMp4StreamInfo
    // 仍报告"成功"，调用方照常把 valid 置 true 发出去。
    bool cancelled = false;
    std::function<void(const std::vector<model::Mp4BoxNode>&)> walk;
    walk = [&](const std::vector<model::Mp4BoxNode>& nodes) {
        for (const auto& node : nodes) {
            if (infrastructure::Checkpoint(cancel)) {
                cancelled = true;
                return;
            }
            if (node.type == "trak") {
                model::ContainerStreamInfo si;
                si.index = result.streams.size();
                std::string handler_type;
                // 在 trak 子节点中提取 tkhd 和 hdlr 信息
                std::function<void(const std::vector<model::Mp4BoxNode>&)> extract_trak;
                extract_trak = [&](const std::vector<model::Mp4BoxNode>& children) {
                    for (const auto& child : children) {
                        if (child.type == "tkhd") {
                            for (const auto& f : child.fields) {
                                if (f.name == "track_id") si.details = "id=" + f.value;
                                if (f.name == "width" && f.value != "0") {
                                    // 查找 height
                                    for (const auto& f2 : child.fields) {
                                        if (f2.name == "height" && f2.value != "0") {
                                            si.details += " " + f.value + "x" + f2.value;
                                            break;
                                        }
                                    }
                                }
                            }
                        } else if (child.type == "mdia") {
                            for (const auto& mdia_child : child.children) {
                                if (mdia_child.type == "hdlr") {
                                    for (const auto& f : mdia_child.fields) {
                                        if (f.name == "handler_type") {
                                            handler_type = f.value;
                                            if (f.value == "vide") si.type = "video";
                                            else if (f.value == "soun") si.type = "audio";
                                            else if (f.value == "text" || f.value == "sbtl") si.type = "subtitle";
                                            else si.type = f.value;
                                        }
                                    }
                                } else if (mdia_child.type == "minf") {
                                    // 深入 minf -> stbl -> stsd
                                    std::function<void(const std::vector<model::Mp4BoxNode>&)> find_stsd;
                                    find_stsd = [&](const std::vector<model::Mp4BoxNode>& stbl_nodes) {
                                        for (const auto& stbl_n : stbl_nodes) {
                                            if (stbl_n.type == "stsd") {
                                                // stsd 的子节点就是编解码器描述
                                                for (const auto& codec_node : stbl_n.children) {
                                                    si.codec = codec_node.type;  // avc1, hvc1, mp4a, av01, vp09 等
                                                    // AV1/VP9 等补充可读编码名: "AV1 (av01)"
                                                    std::string readable = Mp4FourccToName(codec_node.type);
                                                    if (!readable.empty()) {
                                                        si.codec = readable + " (" + codec_node.type + ")";
                                                    }
                                                    // 提取编解码器字段中的关键参数
                                                    for (const auto& cf : codec_node.fields) {
                                                        if (cf.name == "width" && !cf.value.empty()) {
                                                            if (si.details.find("x") != std::string::npos) {
                                                                // 替换 tkhd 的粗略尺寸
                                                                const size_t idx = si.details.find("x");
                                                                size_t start = idx;
                                                                while (start > 0 && si.details[start - 1] != ' ') {
                                                                    --start;
                                                                }
                                                                si.details = si.details.substr(0, start) +
                                                                             cf.value + "x";
                                                                // 查找 height
                                                                for (const auto& cf2 : codec_node.fields) {
                                                                    if (cf2.name == "height") {
                                                                        si.details += cf2.value;
                                                                        break;
                                                                    }
                                                                }
                                                            } else {
                                                                si.details += " " + cf.value + "x";
                                                                for (const auto& cf2 : codec_node.fields) {
                                                                    if (cf2.name == "height") {
                                                                        si.details += cf2.value;
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                        }
                                                        if (cf.name == "sample_rate" && !cf.value.empty()) {
                                                            si.details += " " + cf.value + "Hz";
                                                        }
                                                        if (cf.name == "channel_count" && !cf.value.empty()) {
                                                            si.details += " " + cf.value + "ch";
                                                        }
                                                    }
                                                }
                                                return;
                                            }
                                            find_stsd(stbl_n.children);
                                        }
                                    };
                                    find_stsd(mdia_child.children);
                                }
                            }
                        }
                    }
                };
                extract_trak(node.children);
                si.details = TrimCopy(si.details);
                result.streams.push_back(si);
            }
            walk(node.children);
        }
    };
    walk(box_tree);
    return !cancelled;
}

bool ContainerStructureAnalyzer::ExtractEbmlStreamInfo(const model::EbmlAnalysisResult& ebml_detail,
                                                         model::ContainerStructureResult& result,
                                                         const std::atomic<bool>* cancel) {
    for (const auto& track : ebml_detail.tracks) {
        // 取消必须冒泡到**返回值**（与 ExtractMp4StreamInfo 同理）：以前只管往
        // result.streams 里塞，外层拿到半份流表照样置 valid=true 发布出去。
        if (infrastructure::Checkpoint(cancel)) return false;
        model::ContainerStreamInfo si;
        si.index = track.track_number;
        si.type = track.track_type_name;
        si.codec = track.codec_name;

        // 构建丰富的 details 字符串
        std::vector<std::string> parts;
        if (track.track_type == 1) {  // video
            if (track.pixel_width > 0 && track.pixel_height > 0) {
                parts.push_back(std::to_string(track.pixel_width) + "x"
                                + std::to_string(track.pixel_height));
            }
            if (track.frame_rate > 0) {
                parts.push_back(Format("%.2f fps", track.frame_rate));
            }
        } else if (track.track_type == 2) {  // audio
            if (track.sampling_frequency > 0) {
                parts.push_back(std::to_string(static_cast<long long>(track.sampling_frequency))
                                + " Hz");
            }
            if (track.channels > 0) {
                parts.push_back(std::to_string(track.channels) + " ch");
            }
            if (track.bit_depth > 0) {
                parts.push_back(std::to_string(track.bit_depth) + " bit");
            }
        }
        if (!track.language.empty() && track.language != "und") {
            parts.push_back(track.language);
        }
        if (!track.track_name.empty()) {
            parts.push_back(track.track_name);
        }
        si.details = Join(parts, " ");
        result.streams.push_back(si);
    }

    if (infrastructure::Checkpoint(cancel)) return false;

    // 提取 EBML 元数据
    if (!ebml_detail.title.empty()) result.metadata["title"] = ebml_detail.title;
    if (!ebml_detail.muxing_app.empty()) result.metadata["muxing_app"] = ebml_detail.muxing_app;
    if (!ebml_detail.writing_app.empty()) result.metadata["writing_app"] = ebml_detail.writing_app;
    if (ebml_detail.duration_seconds > 0) {
        result.metadata["duration"] =
            StrCat("%1s", Fixed(ebml_detail.duration_seconds, 2));
    }
    return true;
}

bool ContainerStructureAnalyzer::ConvertMp4Tree(const std::vector<model::Mp4BoxNode>& nodes,
                                                  int depth,
                                                  std::vector<model::ContainerElement>& out,
                                                  const std::atomic<bool>* cancel) {
    for (const auto& node : nodes) {
        if (infrastructure::Checkpoint(cancel)) return false;
        model::ContainerElement elem;
        elem.name = node.type;
        elem.type = "Box";
        elem.size = node.size;
        elem.offset = node.offset;
        elem.depth = depth;

        // value 列：优先展示关键字段，其后补充其余字段（保证 box 内部信息在树中可见）
        auto isKeyField = [](const std::string& n) {
            return n == "handler_type" || n == "major_brand" ||
                   n == "timescale" || n == "duration" ||
                   n == "width" || n == "height" ||
                   n == "entry_count" || n == "sample_rate" ||
                   n == "channel_count" || n == "version" ||
                   n == "flags" || n == "creation_time" ||
                   n == "modification_time" || n == "track_id" ||
                   n == "language" || n == "compatible_brands" ||
                   n == "data_format" || n == "codec";
        };
        std::string key_props;    // 关键字段（放前面）
        std::string rest_props;   // 其余字段
        std::string all_props;    // 全部字段（供 extra/tooltip）
        for (const auto& f : node.fields) {
            const std::string kv = f.name + "=" + f.value;
            if (!all_props.empty()) all_props += "\n";
            all_props += kv;
            if (isKeyField(f.name)) {
                if (!key_props.empty()) key_props += " | ";
                key_props += kv;
            } else {
                if (!rest_props.empty()) rest_props += " | ";
                rest_props += kv;
            }
        }
        std::string value = key_props;
        if (!rest_props.empty()) {
            if (!value.empty()) value += " | ";
            value += rest_props;
        }
        // value 列长度限制，避免超长字段撑爆列宽；完整内容放 extra 供 tooltip 展示
        if (value.size() > 240) value = value.substr(0, 237) + "...";
        elem.value = value;
        elem.extra = all_props;

        if (!ConvertMp4Tree(node.children, depth + 1, elem.children, cancel)) return false;
        out.push_back(elem);
    }
    return true;
}

bool ContainerStructureAnalyzer::ConvertEbmlTree(const std::vector<model::EbmlElementNode>& nodes,
                                                   int depth,
                                                   std::vector<model::ContainerElement>& out,
                                                   const std::atomic<bool>* cancel) {
    for (const auto& node : nodes) {
        if (infrastructure::Checkpoint(cancel)) return false;
        model::ContainerElement elem;
        elem.name = node.name;
        elem.type = "EBML";
        elem.size = node.size;
        elem.offset = node.startOffset();
        elem.depth = depth;
        elem.value = node.value;
        elem.extra = node.extra;

        if (!ConvertEbmlTree(node.children, depth + 1, elem.children, cancel)) return false;
        out.push_back(elem);
    }
    return true;
}

bool ContainerStructureAnalyzer::AnalyzeWithFFmpeg(const std::string& file_path,
                                                    model::ContainerStructureResult& result,
                                                    std::shared_ptr<std::atomic<bool>> cancel) {
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        result.format = model::ContainerFormat::FFmpeg_Generic;
        result.format_name = "Generic";
        result.error_message = "无法分配解封装上下文";
        result.valid = false;
        return false;
    }

    // 安装中断回调: 打开/探测阶段带绝对超时, 关闭流程(CancelAll)置标志后能及时退出,
    // 不再让后台 std::thread 卡在 FFmpeg 阻塞 IO 上、导致 WaitForAll 在 join 时挂死。
    ffmpeg_io::AvInterruptState interrupt;
    interrupt.cancel = cancel.get();
    ffmpeg_io::AttachInterrupt(fmt_ctx, interrupt, ffmpeg_io::kOpenTimeoutUs);

    int ret = avformat_open_input(&fmt_ctx, file_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        result.format = model::ContainerFormat::FFmpeg_Generic;
        result.format_name = "Generic";
        result.error_message = "FFmpeg 无法打开文件";
        result.valid = false;
        return false;
    }

    // 探测阶段允许更长时间, 但仍受取消/超时约束
    interrupt.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;
    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        result.error_message = "FFmpeg 无法获取流信息";
        result.valid = false;
        return false;
    }

    result.format = model::ContainerFormat::FFmpeg_Generic;
    result.format_name = fmt_ctx->iformat->name ? fmt_ctx->iformat->name : "Generic";
    result.file_path = file_path;

    // 构建通用结构树
    model::ContainerElement root;
    root.name = StrCat("%1 Container", ToUpperCopy(result.format_name))
                    ;
    root.type = "Container";
    root.size = fmt_ctx->pb ? avio_size(fmt_ctx->pb) : 0;
    root.offset = 0;
    root.depth = 0;

    // 流信息
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        AVStream* st = fmt_ctx->streams[i];
        model::ContainerElement stream_elem;
        const char* codec_type_name = av_get_media_type_string(st->codecpar->codec_type);
        stream_elem.name =
            StrCat("Stream #%1 (%2)", i, codec_type_name ? codec_type_name : "unknown")
                ;
        stream_elem.type = "Stream";
        stream_elem.depth = 1;

        const char* codec_name = avcodec_get_name(st->codecpar->codec_id);
        stream_elem.value =
            StrCat("codec=%1", codec_name ? codec_name : "?");

        model::ContainerStreamInfo si;
        si.index = i;
        si.type = codec_type_name ? codec_type_name : "unknown";
        si.codec = codec_name ? codec_name : "?";
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            si.details =
                StrCat("%1x%2", st->codecpar->width, st->codecpar->height)
                    ;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            si.details = StrCat("%1 Hz, %2 ch", st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels)
                            ;
        }
        result.streams.push_back(si);

        root.children.push_back(stream_elem);
    }

    // Metadata
    AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        result.metadata[tag->key] = tag->value;

        model::ContainerElement meta_elem;
        meta_elem.name = tag->key;
        meta_elem.type = "Metadata";
        meta_elem.depth = 1;
        meta_elem.value = tag->value;
        root.children.push_back(meta_elem);
    }

    // Chapters
    for (unsigned int i = 0; i < fmt_ctx->nb_chapters; ++i) {
        AVChapter* ch = fmt_ctx->chapters[i];
        model::ContainerElement ch_elem;
        ch_elem.name = "Chapter #" + std::to_string(i);
        ch_elem.type = "Chapter";
        ch_elem.depth = 1;
        double start_sec = ch->start * av_q2d(ch->time_base);
        double end_sec = ch->end * av_q2d(ch->time_base);
        ch_elem.value =
            std::string("start=%.2fs end=%.2fs");

        // Chapter metadata
        AVDictionaryEntry* ch_tag = nullptr;
        while ((ch_tag = av_dict_get(ch->metadata, "", ch_tag, AV_DICT_IGNORE_SUFFIX))) {
            result.metadata["chapter" + std::to_string(i) + "_" + ch_tag->key] = ch_tag->value;
        }

        root.children.push_back(ch_elem);
    }

    result.element_tree.push_back(root);
    result.valid = true;
    result.summary = (StrCat("%1 | %2 流 | %3 章节", ToUpperCopy(result.format_name), fmt_ctx->nb_streams, fmt_ctx->nb_chapters))
                         ;

    avformat_close_input(&fmt_ctx);
    return true;
}

ContainerStructureAnalyzer::StageStatus ContainerStructureAnalyzer::AnalyzeStreamingManifest(
    const std::string& file_path,
    model::ContainerStructureResult& result,
    const std::atomic<bool>* cancel) {
    VE_PERF("AnalyzeStreamingManifest");
    const std::string path = file_path;
    model::StreamingPackageResult& pkg = result.streaming_package;

    bool ok = false;
    if (result.format == model::ContainerFormat::DASH) {
        DashManifestAnalyzer dash;
        // 取消令牌必须传下去: 大清单解析是这一段里最耗时的一环，
        // 不传的话"点取消"要等整份 MPD/m3u8 读完才生效。
        ok = dash.AnalyzeFile(path, pkg, DashManifestOptions{}, cancel);
    } else {
        HlsManifestAnalyzer hls;
        ok = hls.AnalyzeFile(path, pkg, HlsManifestOptions{}, cancel);
    }
    // 取消比失败优先：pkg 里只有半份数据时报"无法解析清单"，
    // 会把用户主动取消说成文件有问题。
    if (infrastructure::Checkpoint(cancel)) return StageStatus::kCancelled;
    if (!ok) {
        result.valid = false;
        result.error_message = pkg.error_message;
        return StageStatus::kFailed;
    }

    // 分片级 / ladder 级交叉校验。fMP4 分片在这里被 Mp4SampleTableAnalyzer 解析，
    // MPEG-TS 分片的逐包解析要依赖 Qt，放到下面的 ProbeTsSegments。
    //
    // 返回值必须接住：以前只查全局取消标志, 于是"分片校验自己判了取消却返回 kDone"、
    // 或"校验阶段失败"两种情况都会让本函数继续往下建树, 最后照样置 valid=true ——
    // 阶段失败被外层悄悄吞掉, 用户拿到一份没做过分片校验的结果。
    // 注意: 两个 StageStatus 是各自类里的独立枚举(不是同一个类型), 只能逐个映射,
    // 不能拿 == 直接比。
    switch (SegmentQcAnalyzer::Analyze(pkg, SegmentQcOptions{}, cancel)) {
    case SegmentQcAnalyzer::StageStatus::kCancelled:
        return StageStatus::kCancelled;
    case SegmentQcAnalyzer::StageStatus::kFailed:
        return StageStatus::kFailed;
    case SegmentQcAnalyzer::StageStatus::kDone:
        break;
    }
    if (infrastructure::Checkpoint(cancel)) return StageStatus::kCancelled;

    if (!ProbeTsSegments(result, cancel)) return StageStatus::kCancelled;
    if (!BuildStreamingTree(result, cancel)) return StageStatus::kCancelled;

    // 建完树再复查一次：树本身是纯拼装，但同样可能在一万个分片的循环里被取消。
    if (infrastructure::Checkpoint(cancel)) return StageStatus::kCancelled;

    result.valid = true;
    return StageStatus::kDone;
}

bool ContainerStructureAnalyzer::ProbeTsSegments(model::ContainerStructureResult& result,
                                                 const std::atomic<bool>* cancel) {
    model::StreamingPackageResult& pkg = result.streaming_package;
    constexpr int kMaxTsProbe = 3;  // 抽查头几个就够了：分片是同一次切片产出的
    int probed = 0;

    auto probe_one = [&](model::SegmentInfo& seg) {
        if (infrastructure::Checkpoint(cancel)) return;
        if (probed >= kMaxTsProbe) return;
        if (seg.partial || seg.container != model::SegmentContainer::MPEG_TS) return;
        if (!seg.exists || seg.resolved_path.empty()) return;
        ++probed;
        TsStructureAnalyzer ts;
        model::ContainerStructureResult ts_result;
        // 取消令牌跟着走：TS 分片解析要逐包扫几百到几千个 TS 包，
        // 大型包这里能跑出好几秒，取消只是"抽查头三个"时也已经晚了。
        if (ts.Analyze((seg.resolved_path), ts_result, cancel)) {
            seg.probed = true;
            return;
        }
        seg.container_parse_failed = true;
        seg.container_error = ts_result.error_message.empty()
                                  ? std::string("TS 结构解析失败")
                                  : ts_result.error_message;
    };

    for (model::MediaPlaylistInfo& pl : pkg.playlists) {
        for (model::SegmentInfo& seg : pl.segments) {
            if (infrastructure::Checkpoint(cancel)) return false;
            probe_one(seg);
        }
    }
    for (model::DashRepresentationInfo& rep : pkg.representations) {
        for (model::SegmentInfo& seg : rep.segments) {
            if (infrastructure::Checkpoint(cancel)) return false;
            probe_one(seg);
        }
    }
    return true;
}

bool ContainerStructureAnalyzer::BuildStreamingTree(model::ContainerStructureResult& result,
                                                    const std::atomic<bool>* cancel) {
    const model::StreamingPackageResult& pkg = result.streaming_package;
    // 分段复查取消：这一函数是纯拼装，但外层规模是由清单决定的（几千个 playlist 的
    // master 完全可能存在），拼到一半被取消时不能把半棵树交给 UI。
    const auto cancelled = [cancel] { return infrastructure::Checkpoint(cancel); };

    // 入参保持 std::string（调用方大量用 StrCat 拼装），落地到 domain 时统一转 std::string
    auto make_elem = [](const std::string& name, const std::string& type, int depth,
                        const std::string& value = std::string(),
                        const std::string& extra = std::string()) {
        model::ContainerElement e;
        e.name = name;
        e.type = type;
        e.depth = depth;
        e.value = value;
        e.extra = extra;
        return e;
    };

    model::ContainerElement root = make_elem((model::ToString(pkg.kind)),
                                             "Manifest", 0);
    root.extra = pkg.manifest_path;

    if (pkg.IsDash()) {
        root.value = (StrCat("%1 | %2 Period | %3 Representation", (pkg.mpd_type), pkg.periods.size(), pkg.representations.size()))
                         ;
        for (const model::DashPeriodInfo& period : pkg.periods) {
            if (cancelled()) return false;
            model::ContainerElement period_elem =
                make_elem(StrCat("Period %1", period.index), "Period", 1,
                          StrCat("duration=%1s", Fixed(period.duration_seconds, 3)));
            for (const model::DashAdaptationSetInfo& as : period.adaptation_sets) {
                model::ContainerElement as_elem =
                    make_elem(StrCat("AdaptationSet %1", as.index), "AdaptationSet", 2,
                              (as.content_type + " " + as.mime_type));
                for (int ri : as.representation_indices) {
                    if (ri < 0 || static_cast<size_t>(ri) >= pkg.representations.size()) continue;
                    const model::DashRepresentationInfo& rep = pkg.representations[ri];
                    model::ContainerElement rep_elem = make_elem(
                        StrCat("Representation %1", (rep.id)),
                        "Representation", 3,
                        StrCat("%1x%2 %3 kbps", rep.width, rep.height, rep.bandwidth_bps / 1000));
                    rep_elem.extra = rep.codecs;
                    constexpr int kMaxSegmentNodes = 100;
                    for (int i = 0; i < static_cast<int>(rep.segments.size()) && i < kMaxSegmentNodes;
                         ++i) {
                        const model::SegmentInfo& seg = rep.segments[i];
                        rep_elem.children.push_back(make_elem(
                            StrCat("Segment %1", seg.sequence), "Segment", 4,
                            StrCat("t=%1s d=%2s %3", Fixed(seg.start_seconds, 3), Fixed(seg.duration_seconds, 3), seg.exists ? std::string("ok") : std::string("缺失")),
                            (seg.uri)));
                    }
                    if (static_cast<int>(rep.segments.size()) > kMaxSegmentNodes) {
                        rep_elem.children.push_back(
                            make_elem(StrCat("... 其余 %1 个分片省略", static_cast<int>(rep.segments.size()) - kMaxSegmentNodes),
                                      "Segment", 4));
                    }
                    as_elem.children.push_back(rep_elem);
                }
                period_elem.children.push_back(as_elem);
            }
            root.children.push_back(period_elem);
        }

        // 流信息：视频 ladder + 音频 representation
        for (const model::StreamingLadderEntry& e : pkg.ladder) {
            model::ContainerStreamInfo si;
            si.index = result.streams.size();
            si.type = "video";
            si.codec = e.video_codec;
            si.details = (StrCat("%1x%2 %3 kbps", e.width, e.height, e.bandwidth_bps / 1000))
                             ;
            result.streams.push_back(si);
        }
        for (const model::DashRepresentationInfo& rep : pkg.representations) {
            if (rep.content_type != "audio") continue;
            model::ContainerStreamInfo si;
            si.index = result.streams.size();
            si.type = "audio";
            si.codec = rep.audio_codec;
            si.details =
                StrCat("%1 kbps", rep.bandwidth_bps / 1000);
            result.streams.push_back(si);
        }

        result.metadata["MPD type"] = pkg.mpd_type;
        if (pkg.media_presentation_duration_s > 0.0) {
            result.metadata["时长"] =
                StrCat("%1 s", Fixed(pkg.media_presentation_duration_s, 3))
                    ;
        }
        result.summary = (StrCat("DASH | %1 Period | %2 Representation | %3 分片", pkg.periods.size(), pkg.representations.size(), static_cast<unsigned long long>(pkg.TotalSegments())))
                             ;
    } else {
        root.value = (StrCat("%1 | %2 variant | %3 playlist",
                             (pkg.kind == model::StreamingKind::HlsMaster ? "master" : "media"),
                             pkg.variants.size(), pkg.playlists.size()));

        model::ContainerElement variants_elem =
            make_elem(StrCat("Variants (%1)", pkg.variants.size()), "Group", 1);
        for (const model::HlsVariantInfo& v : pkg.variants) {
            if (cancelled()) return false;
            model::ContainerElement v_elem = make_elem(
                StrCat("variant #%1", v.index), "Variant", 2,
                StrCat("%1 kbps %2", v.bandwidth_bps / 1000, (v.resolution)));
            v_elem.extra = (v.uri + "  codecs=" + v.codecs);
            variants_elem.children.push_back(v_elem);
        }
        root.children.push_back(variants_elem);

        if (!pkg.renditions.empty()) {
            model::ContainerElement rend_elem =
                make_elem(StrCat("Renditions (%1)", pkg.renditions.size()), "Group", 1);
            for (const model::HlsRenditionInfo& r : pkg.renditions) {
                rend_elem.children.push_back(make_elem(
                    (r.type + " " + r.name), "Rendition", 2,
                    ("group=" + r.group_id +
                                           (r.language.empty() ? "" : " lang=" + r.language))));
            }
            root.children.push_back(rend_elem);
        }

        model::ContainerElement pl_elem =
            make_elem(StrCat("Media playlists (%1)", pkg.playlists.size()), "Group", 1);
        constexpr int kMaxSegmentNodes = 100;
        for (const model::MediaPlaylistInfo& pl : pkg.playlists) {
            if (cancelled()) return false;
            model::ContainerElement one =
                make_elem(StrCat("%1 #%2", (pl.role), pl.index),
                          "Playlist", 2,
                          StrCat("%1 分片 target=%2s", pl.SegmentCount(), pl.target_duration_s));
            one.extra = pl.uri;
            for (int i = 0; i < static_cast<int>(pl.segments.size()) && i < kMaxSegmentNodes; ++i) {
                const model::SegmentInfo& seg = pl.segments[i];
                one.children.push_back(make_elem(
                    seg.partial ? std::string("Part") : StrCat("Segment %1", seg.sequence),
                    "Segment", 3,
                    StrCat("t=%1s d=%2s %3%4", Fixed(seg.start_seconds, 3), Fixed(seg.duration_seconds, 3), seg.exists ? std::string("ok") : std::string("缺失"), seg.discontinuity_before ? " [discontinuity]" : ""),
                    (seg.uri)));
            }
            if (static_cast<int>(pl.segments.size()) > kMaxSegmentNodes) {
                one.children.push_back(make_elem(
                    StrCat("... 其余 %1 个分片省略", static_cast<int>(pl.segments.size()) - kMaxSegmentNodes),
                    "Segment", 3));
            }
            pl_elem.children.push_back(one);
        }
        root.children.push_back(pl_elem);

        for (const model::StreamingLadderEntry& e : pkg.ladder) {
            model::ContainerStreamInfo si;
            si.index = result.streams.size();
            si.type = e.width > 0 ? "video" : "audio";
            si.codec = (e.video_codec.empty() ? e.audio_codec : e.video_codec);
            si.details = StrCat("%1x%2 %3 kbps", e.width, e.height, e.bandwidth_bps / 1000);
            result.streams.push_back(si);
        }

        for (const model::MediaPlaylistInfo& pl : pkg.playlists) {
            if (pl.has_target_duration) {
                result.metadata["EXT-X-TARGETDURATION #" + std::to_string(pl.index)] =
                    StrCat("%1 s", pl.target_duration_s);
            }
            if (pl.encrypted) {
                result.metadata["加密 #" + std::to_string(pl.index)] =
                    pl.key_method;
            }
        }
        result.metadata["点播/直播"] = pkg.IsLive() ? "直播" : "点播";
        result.metadata["低延迟 HLS"] = pkg.playlists.empty() ? "否"
            : (std::any_of(pkg.playlists.begin(), pkg.playlists.end(),
                           [](const model::MediaPlaylistInfo& p) { return p.low_latency; })
                   ? "是"
                   : "否");
        result.summary = (StrCat("HLS | %1 variant | %2 playlist | %3 分片", pkg.variants.size(), pkg.playlists.size(), static_cast<unsigned long long>(pkg.TotalSegments())))
                             ;
    }

    if (cancelled()) return false;  // 树拼完了但取消也到了: 半棵树照样不算结果

    result.element_tree.push_back(root);
    return true;
}

void ContainerStructureAnalyzer::Reset() {
    // 无状态, 无需重置
}

} // namespace videoeye
