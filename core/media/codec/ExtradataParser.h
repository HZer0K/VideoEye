#pragma once

// Extradata 解析器主体。纯结果类型在 core/media/codec/ExtradataTypes.h。

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataTypes.h"

namespace videoeye {

// ==========================================================================
// Extradata 解析器
//
// 职责：
//   1. 识别 extradata 封装格式（Annex B / avcC / hvcC / av1C）
//   2. 统一抽象为 NAL/OBU 列表
//   3. 提取配置记录中的基本信息
//   4. 支持 Annex B ↔ length-prefix 转换
// ==========================================================================
class ExtradataParser {
public:
    // 从 extradata 开始解析
    static ExtradataResult Parse(const uint8_t* extradata, size_t size);

    // 直接指定格式解析。syntax 只对 AnnexB / 长度前缀流有意义（见 NalSyntax 注释）；
    // 配置记录（avcC/hvcC/vvcC）里 NAL 类型由数组头给出，不需要猜。
    static ExtradataResult ParseWithFormat(ExtradataFormat format, const uint8_t* data, size_t size,
                                           NalSyntax syntax = NalSyntax::Auto);

    // Annex B 流的 NAL 抽取（VVC 必须传 NalSyntax::Vvc，否则类型会读错）
    static std::vector<NalUnit> ExtractAnnBNalUnits(const uint8_t* data, size_t size,
                                                    NalSyntax syntax = NalSyntax::Auto);

    // 长度前缀流（MP4/MOV/fMP4 的媒体包：`[N 字节长度][NAL]`）的 NAL 抽取。
    //
    // ❗ 不要用 ConvertLengthPrefixToAnnexB 中转：那要多拷贝一遍整包，而且它的
    // 截断是静默 break，调用方分不清"没有 NAL"和"包被截断"。这里直接产出 NalUnit，
    // 截断通过 `truncated` 回传（可传 nullptr）。
    //
    // prefix_bytes 只接受 1 / 2 / 4，其它值直接返回空（合法宽度来自
    // avcC/hvcC 的 lengthSizeMinusOne + 1，不可能是别的值）。
    static std::vector<NalUnit> ExtractLengthPrefixedNalUnits(const uint8_t* data, size_t size, NalSyntax syntax,
                                                              int prefix_bytes, bool* truncated = nullptr);

    // 判断一段字节是否"整段都由长度前缀 NAL 组成"：用于在没有容器信息时
    // 区分 Annex B 包与长度前缀包。要求每个长度都 > 0、不越界，且正好消耗完。
    static bool LooksLikeLengthPrefixed(const uint8_t* data, size_t size, int prefix_bytes);

    // VVC NAL 单元解析（2 字节 header，type = (byte1 >> 3) & 0x1F）
    static NalUnit ParseVvcNalUnit(const uint8_t* data, size_t size);

    // 转换封装格式（Annex B ↔ length-prefix）
    static std::vector<uint8_t> ConvertAnnexBToLengthPrefix(const std::vector<uint8_t>& annex_b);

    static std::vector<uint8_t> ConvertLengthPrefixToAnnexB(const std::vector<uint8_t>& length_prefix,
                                                            int prefix_bytes);

    // 从裸 OBU 流（或 av1C 尾部的 configOBUs）中依次抽出所有 OBU。
    // 对外可见：AV1 的序列头往往在 packet 里而不是 extradata 里，
    // 上层需要自己拿一段 OBU 字节来解析。
    //
    // ⚠️ 会为每个 OBU 分配并复制一份 payload。**逐包**扫描的热路径请用
    // ForEachObuUnit —— 那边零拷贝。
    static std::vector<ObuUnit> ExtractObuUnits(const uint8_t* data, size_t size);

    // 零拷贝遍历一段 OBU 流：回调拿到的是指向 data 的视图，不分配、不复制。
    //
    // Fn 可以是 `void(const ObuView&)`，也可以是 `bool(const ObuView&)`
    // —— 返回 false 立即停止遍历（找到想要的 OBU 就能收工，不必扫完整个包）。
    //
    // 遍历规则与 ExtractObuUnits 完全一致（同一份 ReadObuView），包括
    // "0 长度的 OBU_TEMPORAL_DELIMITER 是合法的" 与 "截断时拿多少算多少"。
    template <typename Fn>
    static void ForEachObuUnit(const uint8_t* data, size_t size, Fn&& fn) {
        if (data == nullptr || size == 0) {
            return;
        }
        const uint8_t* ptr = data;
        size_t remaining = size;
        while (remaining > 0) {
            const uint8_t* before = ptr;
            ObuView view;
            if (!ReadObuView(ptr, remaining, view)) {
                break;
            }
            // 只在指针没有推进时停止，避免死循环（见 ReadObuView 的失败语义）。
            if (ptr <= before) {
                break;
            }
            if constexpr (std::is_same_v<std::invoke_result_t<Fn, const ObuView&>, bool>) {
                if (!fn(view)) {
                    return;
                }
            } else {
                fn(view);
            }
            remaining = static_cast<size_t>(data + size - ptr);
        }
    }

private:
    // 从 ptr 处读一个 OBU 头并算出 payload 区间；成功时把 ptr 推进到下一个 OBU。
    // 返回 false 表示"这里不是合法 OBU 起始"—— 此时不推进 ptr，由调用方决定停止。
    static bool ReadObuView(const uint8_t*& ptr, size_t remaining, ObuView& out);
    // 格式检测
    static ExtradataFormat DetectFormat(const uint8_t* data, size_t size);

    // 具体格式解析器（原样返回，不做「错误状态 ⇒ valid=false」归一化）
    static ExtradataResult ParseWithFormatRaw(ExtradataFormat format, const uint8_t* data, size_t size,
                                              NalSyntax syntax);

    // 具体格式解析器
    static ExtradataResult ParseAnnexB(const uint8_t* data, size_t size, NalSyntax syntax = NalSyntax::Auto);
    static ExtradataResult ParseAvcC(const uint8_t* data, size_t size);
    static ExtradataResult ParseHvcC(const uint8_t* data, size_t size);
    static ExtradataResult ParseVvcC(const uint8_t* data, size_t size);
    static ExtradataResult ParseAv1C(const uint8_t* data, size_t size);
    // vpcC（VP9 的 MP4 配置记录）。WebM 的 VP9 没有 extradata，走带内关键帧解析。
    static ExtradataResult ParseVpcC(const uint8_t* data, size_t size);
    // H.264 NAL 单元解析
    static NalUnit ParseH264NalUnit(const uint8_t* data, size_t size);

    // H.265 NAL 单元解析
    static NalUnit ParseHevcNalUnit(const uint8_t* data, size_t size);

    // AV1 OBU 解析
    static ObuUnit ParseAv1Obu(const uint8_t*& ptr, size_t remaining);

    // Annex B 解析辅助
    static const uint8_t* FindStartCode(const uint8_t* pos, const uint8_t* end);
};

} // namespace videoeye
