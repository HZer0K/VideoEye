#pragma once

#include <algorithm>
#include <cmath>

namespace videoeye {
namespace model {

// 静音/无信号时的哨兵值。
// ebur128 规定积分响度的下界为 -120 LUFS（数字静音即 -inf，约定取 -120），
// 电平类指标（dBFS / dBTP）沿用同一惯例，UI 侧可直接判空。
constexpr double kSilenceLufs = -120.0;
constexpr double kSilenceLevelDb = -120.0;

// 线性幅度 -> dBFS（0 幅度返回哨兵值）
inline double AmplitudeToDb(double amplitude) {
    if (amplitude <= 0.0) return kSilenceLevelDb;
    return 20.0 * std::log10(amplitude);
}

// 均方值(mean square) -> dBFS
inline double PowerToDb(double mean_square) {
    if (mean_square <= 0.0) return kSilenceLevelDb;
    return 10.0 * std::log10(mean_square);
}

// 均方值 -> LUFS（BS.1770: L = -0.691 + 10*log10(加权后均方)）
inline double PowerToLufs(double mean_square) {
    if (mean_square <= 0.0) return kSilenceLufs;
    return std::max(-0.691 + 10.0 * std::log10(mean_square), kSilenceLufs);
}

inline double DbToAmplitude(double db) { return std::pow(10.0, db / 20.0); }

// dB -> 线性功率比（用于门限换算，如 10 LU 门限 -> 0.1）
inline double DbToPower(double db) { return std::pow(10.0, db / 10.0); }

// 响度 / 电平曲线上的一个采样点
//
// 采样步进 = 100 ms：BS.1770 规定 400 ms 块 + 75% 重叠（等效每 100 ms 出一个块），
// 因此该结构同时承载了「瞬时(400ms) / 短期(3s) / 累计积分」三条曲线，
// UI 折线图、CSV 导出与规则判定共用这一份数据，避免多份序列对不齐。
struct LoudnessPoint {
    double timestamp_seconds = 0.0;              // 窗口结束时刻（秒）
    double momentary_lufs = kSilenceLufs;        // 瞬时响度 M（400 ms）
    double short_term_lufs = kSilenceLufs;       // 短期响度 S（3 s）
    double integrated_lufs = kSilenceLufs;       // 到该时刻为止的累计积分响度（含双门限）
    double rms_dbfs = kSilenceLevelDb;           // 窗口内 RMS
    double sample_peak_dbfs = kSilenceLevelDb;   // 窗口内采样峰值
    double true_peak_dbtp = kSilenceLevelDb;     // 窗口内 4× 过采样真峰值（未启用时回落为采样峰值）
    double correlation = 1.0;                    // 窗口内最差声道对相关性 [-1, 1]
    int correlation_ch0 = -1;                    // 最差声道对的两个声道下标
    int correlation_ch1 = -1;
    bool silent = false;                         // 窗口是否落入静音段
};

}  // namespace model
}  // namespace videoeye
