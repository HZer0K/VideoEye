#pragma once

// ==========================================================================
// H.266 (VVC) NAL unit type —— 唯一事实源
//
// 为什么要单独一个头：这三个常量集合原先在三个文件里各写一份
//   * VvcBitstreamParser.cpp  —— VPS(14) / SPS(15) / PPS(16)
//   * VideoEsInspector.cpp    —— SPS / PPS / PH(19) / SEI(23,24) / VCL 上界(11)
//   * VvcSliceHeader.cpp      —— IDR(7) / IDR_N_LP(8) / CRA(9) / GDR(10)
// 三份都不是全集，且"谁是参数集"这件事在参数集追踪器接入时又要用一次。
// 分散的后果不是编译错误，是"新增一处、漏改两处"——新增 OPI/DCI 跟踪时
// 按名字写 `== 12` 而另一处仍在用旧集合，逻辑看着能跑，结论却是漏的。
//
// 数值一律以 H.266 7.4.1 Table 5 为准。0..11 是 VCL（含 dependent slice
// 的 6），12 起是非 VCL。
// ==========================================================================

namespace videoeye {

// ---- VCL（0..11）----
constexpr int kVvcNutTrail = 0;    // TRAIL_NUT
constexpr int kVvcNutStsa = 1;     // STSA_NUT
constexpr int kVvcNutRadl = 2;     // RADL_NUT
constexpr int kVvcNutRasl = 3;     // RASL_NUT
constexpr int kVvcNutIdrWRadl = 7; // IDR_W_RADL
constexpr int kVvcNutIdrNLp = 8;   // IDR_N_LP
constexpr int kVvcNutCra = 9;      // CRA_NUT
constexpr int kVvcNutGdr = 10;     // GDR_NUT
constexpr int kVvcNutVclLast = 11; // RSV_VCL_11，VCL 区间上界（含）

// ---- 非 VCL（12 起）----
constexpr int kVvcNutOpi = 12;       // OPI_NUT
constexpr int kVvcNutDci = 13;       // DCI_NUT
constexpr int kVvcNutVps = 14;       // VPS_NUT
constexpr int kVvcNutSps = 15;       // SPS_NUT
constexpr int kVvcNutPps = 16;       // PPS_NUT
constexpr int kVvcNutPrefixAps = 17; // PREFIX_APS_NUT
constexpr int kVvcNutSuffixAps = 18; // SUFFIX_APS_NUT
constexpr int kVvcNutPh = 19;        // PH_NUT
constexpr int kVvcNutAud = 20;       // AUD_NUT
constexpr int kVvcNutEos = 21;       // EOS_NUT
constexpr int kVvcNutEob = 22;       // EOB_NUT
constexpr int kVvcNutPrefixSei = 23; // PREFIX_SEI_NUT
constexpr int kVvcNutSuffixSei = 24; // SUFFIX_SEI_NUT
constexpr int kVvcNutFd = 25;        // FD_NUT

// VCL 判定：这些 NAL 承载 slice（因此有 slice_header）。
inline bool VvcIsVclNalType(int type) {
    return type >= 0 && type <= kVvcNutVclLast;
}

// 参数集判定（含 APS）：内容变化会影响后续图像的解析，参数集追踪器只认这些。
// OPI / DCI 也在内 —— 它们改变的是"整份流的能力声明"，不是逐图工具参数。
inline bool VvcIsParameterSetNalType(int type) {
    return type == kVvcNutOpi || type == kVvcNutDci || type == kVvcNutVps || type == kVvcNutSps || type == kVvcNutPps ||
           type == kVvcNutPrefixAps || type == kVvcNutSuffixAps;
}

} // namespace videoeye
