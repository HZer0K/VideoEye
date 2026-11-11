#pragma once

// 手写最小 H.264 码流 + 无 moov MP4 容器（测试专用）。
//
// 为什么需要它：仓库锁定的预编译 FFmpeg 没有 libx264，现场生成不出 B 帧；而且
// "SPS 带 VUI timing" / "SPS 没有 VUI" / "VUI 声明非恒定帧率" 这三种组合没有
// 现成语料能覆盖。这里按 H.264 语法逐位写 SPS/PPS/slice_header，只保证能被
// 本项目的解析器读懂 —— **不含可解码的图像数据**，所以只能用结构类断言，
// 不能用 PSNR / 解码帧数之类的内容类断言。
//
// 命名统一加 `H264Test` 前缀并放在全局命名空间：分层 / 命名空间审计脚本
// 对具名子命名空间有目录匹配要求，测试辅助不参与那套规则，但也别撞上。

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

class H264TestBitWriter {
public:
    void U(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i)
            Push(((value >> i) & 1u) != 0);
    }
    void Ue(uint32_t value) {
        const uint32_t code = value + 1;
        int width = 0;
        while ((1u << width) <= code)
            ++width;
        for (int i = 1; i < width; ++i)
            Push(false);
        U(code, width);
    }
    void Se(int32_t value) {
        Ue(value <= 0 ? static_cast<uint32_t>(-2 * value) : static_cast<uint32_t>(2 * value - 1));
    }
    void Trailing() {
        Push(true);
        while ((bits_ % 8) != 0)
            Push(false);
    }
    std::vector<uint8_t> Bytes() const {
        return bytes_;
    }

private:
    void Push(bool bit) {
        if ((bits_ % 8) == 0)
            bytes_.push_back(0);
        if (bit)
            bytes_.back() |= static_cast<uint8_t>(0x80u >> (bits_ % 8));
        ++bits_;
    }

    std::vector<uint8_t> bytes_;
    std::size_t bits_ = 0;
};

// RBSP → EBSP：连续两个 0 之后出现 00..03 时插入 0x03，否则会被当成起始码。
inline std::vector<uint8_t> H264TestEscapeRbsp(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out;
    int zeros = 0;
    for (uint8_t byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

struct H264TestSpsOptions {
    // false = 完全不写 VUI（模拟"没有帧率证据"的源）。
    bool with_vui = true;
    bool with_timing = true;
    // false = 码流自己声明非恒定帧率（fixed_frame_rate_flag=0）。
    bool fixed_frame_rate = true;
    uint32_t num_units_in_tick = 1;
    uint32_t time_scale = 50; // 默认 25 fps：每帧 2 tick
    // 32 位字段写坏用的口子：true 时把 num_units_in_tick 写成 0。
    bool zero_timing = false;
};

// baseline profile、无 cropping、4:4:0 简化的最小 SPS；尺寸 320x176。
inline std::vector<uint8_t> H264TestMakeSps(const H264TestSpsOptions& options = H264TestSpsOptions{}) {
    H264TestBitWriter writer;
    writer.U(66, 8);                         // profile_idc = baseline
    writer.U(0, 8);                          // constraint flags
    writer.U(30, 8);                         // level_idc
    writer.Ue(0);                            // seq_parameter_set_id
    writer.Ue(0);                            // log2_max_frame_num_minus4 → 4 位
    writer.Ue(0);                            // pic_order_cnt_type
    writer.Ue(0);                            // log2_max_pic_order_cnt_lsb_minus4 → 4 位
    writer.Ue(1);                            // max_num_ref_frames
    writer.U(0, 1);                          // gaps_in_frame_num_value_allowed_flag
    writer.Ue(19);                           // pic_width_in_mbs_minus1  = 320/16-1
    writer.Ue(10);                           // pic_height_in_map_units_minus1 = 176/16-1
    writer.U(1, 1);                          // frame_mbs_only_flag
    writer.U(1, 1);                          // direct_8x8_inference_flag
    writer.U(0, 1);                          // frame_cropping_flag
    writer.U(options.with_vui ? 1u : 0u, 1); // vui_parameters_present_flag
    if (options.with_vui) {
        writer.U(0, 1);                             // aspect_ratio_info_present_flag
        writer.U(0, 1);                             // overscan_info_present_flag
        writer.U(0, 1);                             // video_signal_type_present_flag
        writer.U(0, 1);                             // chroma_loc_info_present_flag
        writer.U(options.with_timing ? 1u : 0u, 1); // timing_info_present_flag
        if (options.with_timing) {
            writer.U(options.zero_timing ? 0u : options.num_units_in_tick, 32);
            writer.U(options.time_scale, 32);
            writer.U(options.fixed_frame_rate ? 1u : 0u, 1); // fixed_frame_rate_flag
        }
        writer.U(0, 1); // nal_hrd_parameters_present_flag
        writer.U(0, 1); // vcl_hrd_parameters_present_flag
        writer.U(0, 1); // pic_struct_present_flag
        writer.U(0, 1); // bitstream_restriction_flag
    }
    writer.Trailing();
    std::vector<uint8_t> nal{0x67};
    const std::vector<uint8_t> escaped = H264TestEscapeRbsp(writer.Bytes());
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

inline std::vector<uint8_t> H264TestMakePps() {
    H264TestBitWriter writer;
    writer.Ue(0);   // pic_parameter_set_id
    writer.Ue(0);   // seq_parameter_set_id
    writer.U(0, 1); // entropy_coding_mode_flag
    writer.U(0, 1); // bottom_field_pic_order_in_frame_present_flag
    writer.Ue(0);   // num_slice_groups_minus1
    writer.Ue(0);   // num_ref_idx_l0_default_active_minus1
    writer.Ue(0);   // num_ref_idx_l1_default_active_minus1
    writer.U(0, 1); // weighted_pred_flag
    writer.U(0, 2); // weighted_bipred_idc
    writer.Se(0);   // pic_init_qp_minus26
    writer.Se(0);   // pic_init_qs_minus26
    writer.Se(0);   // chroma_qp_index_offset
    writer.U(0, 1); // deblocking_filter_control_present_flag
    writer.U(0, 1); // constrained_intra_pred_flag
    writer.U(0, 1); // redundant_pic_cnt_present_flag
    writer.Trailing();
    std::vector<uint8_t> nal{0x68};
    const std::vector<uint8_t> escaped = H264TestEscapeRbsp(writer.Bytes());
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

// slice_type：2 = I、0 = P、1 = B。追踪器只读到 poc_lsb 为止，
// 后面的参考列表 / 残差语法对 POC 没有影响，这里不写（也写不出可解码图像）。
inline std::vector<uint8_t> H264TestMakeSlice(bool idr, uint32_t slice_type, uint32_t frame_num, uint32_t poc_lsb) {
    H264TestBitWriter writer;
    writer.Ue(0); // first_mb_in_slice
    writer.Ue(slice_type);
    writer.Ue(0); // pic_parameter_set_id
    writer.U(frame_num, 4);
    if (idr)
        writer.Ue(0); // idr_pic_id
    writer.U(poc_lsb, 4);
    writer.Trailing();
    std::vector<uint8_t> nal{static_cast<uint8_t>(idr ? 0x65 : 0x41)};
    const std::vector<uint8_t> escaped = H264TestEscapeRbsp(writer.Bytes());
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

inline void H264TestAppendNal(std::vector<uint8_t>* out, const std::vector<uint8_t>& nal) {
    const uint32_t size = static_cast<uint32_t>(nal.size());
    out->push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
    out->push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
    out->push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(size & 0xFF));
    out->insert(out->end(), nal.begin(), nal.end());
}

inline void H264TestAppendU32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out->push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(value & 0xFF));
}

// ftyp + mdat（无 moov）。gops 个 GOP，每个 GOP 写 SPS/PPS + 一组解码序帧。
// reordered=true 时每 GOP 是 I P B B（解码序 POC = 0 6 2 4）。
inline bool H264TestWriteAvccMp4(const std::string& path, const H264TestSpsOptions& sps_options, int gops = 2,
                                 bool reordered = true) {
    std::vector<uint8_t> payload;
    const std::vector<uint8_t> sps = H264TestMakeSps(sps_options);
    const std::vector<uint8_t> pps = H264TestMakePps();
    for (int gop = 0; gop < gops; ++gop) {
        H264TestAppendNal(&payload, sps);
        H264TestAppendNal(&payload, pps);
        H264TestAppendNal(&payload, H264TestMakeSlice(true, 2, 0, 0)); // I  POC 0
        if (reordered) {
            H264TestAppendNal(&payload, H264TestMakeSlice(false, 0, 1, 6)); // P  POC 6
            H264TestAppendNal(&payload, H264TestMakeSlice(false, 1, 2, 2)); // B  POC 2
            H264TestAppendNal(&payload, H264TestMakeSlice(false, 1, 3, 4)); // B  POC 4
        } else {
            H264TestAppendNal(&payload, H264TestMakeSlice(false, 0, 1, 2)); // P  POC 2
            H264TestAppendNal(&payload, H264TestMakeSlice(false, 0, 2, 4)); // P  POC 4
            H264TestAppendNal(&payload, H264TestMakeSlice(false, 0, 3, 6)); // P  POC 6
        }
    }

    std::vector<uint8_t> file;
    H264TestAppendU32(&file, 20);
    file.insert(file.end(), {'f', 't', 'y', 'p'});
    file.insert(file.end(), {'i', 's', 'o', 'm'});
    H264TestAppendU32(&file, 512);
    file.insert(file.end(), {'i', 's', 'o', 'm'});
    H264TestAppendU32(&file, static_cast<uint32_t>(payload.size() + 8));
    file.insert(file.end(), {'m', 'd', 'a', 't'});
    file.insert(file.end(), payload.begin(), payload.end());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}
