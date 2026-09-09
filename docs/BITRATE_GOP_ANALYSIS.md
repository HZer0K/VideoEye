# 码率与 GOP 深度分析

> 关联需求：功能 2（码率与 GOP 深度分析）
> 状态：已实现（2026-09-09）

码率与 GOP 决定编码质量、播放缓冲、seek 体验和平台合规。本模块把"逐包大小 + 时间戳"
换算成滑动窗口码率，并重建 GOP 结构，最后给出异常清单与编码优化建议。

## 1. 模块落点

| 文件 | 职责 |
| --- | --- |
| `core/model/BitratePoint.h/.cpp` | 帧类型枚举 `FrameType`、滑动窗口码率采样点 `BitratePoint` |
| `core/model/GopInfo.h/.cpp` | 单条 GOP 记录 `GopInfo`（起止时间/帧数/字节/I-P-B/closed/最大帧） |
| `core/analyzer/BitrateGopAnalyzer.h/.cpp` | 分析器本体：窗口码率、I/P/B、GOP 维护、异常识别、场景关联 |
| `core/analyzer/AnalysisCoordinator.cpp` | 在既有全文件 demux 循环中喂数据（无额外 I/O） |
| `core/model/QcModels.cpp` + `core/analyzer/QcRuleEngine.cpp` | 6 条 QC 规则，异常进入诊断报告 |
| `ui/analysis_panel/AnalysisPanel.*` | 「码率与 GOP」页：曲线 / GOP 表 / 异常 / 建议 + CSV 导出 |
| `tests/unit/test_bitrate_gop_analyzer.cpp` | 10 个 gtest 用例，覆盖 4.6 四项验收 |

### 设计约束

`BitrateGopAnalyzer` **不依赖 FFmpeg 也不依赖 Qt**：时间戳由调用方换算成秒，帧类型用
`model::FrameType`（取值与 `AVPictureType` 对齐）。好处是单测可以直接喂合成样本，
无需真实媒体文件，也不必链接 FFmpeg。

## 2. 数据契约

### 2.1 滑动窗口码率

- 样本按 **PTS 排序**后统计（B 帧存在时解码顺序 ≠ 显示顺序，按 DTS 统计会错位）。
- 窗口长度默认 `{1.0, 0.5, 2.0, 5.0}` 秒，第一项为默认窗口；UI 下拉框直接遍历此列表。
- 步进 `hop = window / 4`（75% 重叠），曲线比"逐秒分桶"平滑，峰值更接近真实 VBV 表现。
- 单点码率 = 窗口内字节数 × 8 / window / 1000。**窗口长度固定**，尾部不足一个窗口的
  部分仍按整窗计（避免尾部虚高）。
- 点数上限 `kMaxCurvePoints = 20000`，超出时自动加大 hop。

**窗口越短峰值越高**：0.5 s 窗口能暴露单帧级别的突发，5 s 窗口反映持续带宽占用。
验收样本实测 0.5 s 峰值 21760 kbps vs 5 s 峰值 9472 kbps，两者应结合看。

### 2.2 GOP 口径

- GOP 以**关键帧**（IDR / CRA / recovery point）开始，到下一个关键帧之前结束。
- 因此 `gops.size()` 通常等于关键帧数；最后一个 GOP 若未被收尾，`complete = false`。
- `closed_gop`：以 IDR 开头记 `true`；文件开头就不是关键帧时记 `false`。
  目前**不区分 IDR 与 CRA**（需要 bitstream 层 NAL 解析，未实现），
  `closed_gop` 实际表达的是"该 GOP 起点可独立解码"。
- 统计口径：
  - `gop_duration_*` 用全部 GOP
  - `key_interval_*` / `key_interval_irregularity` 只用 `complete == true` 的 GOP

### 2.3 帧类型来源（三档）

| 档位 | 触发条件 | I/P/B 准确性 |
| --- | --- | --- |
| codec parser（**默认**） | `decode_frame_types = false` | 解析器给出 pict_type；部分封装/编码可能返回 `NONE` |
| 解码器（可选） | `decode_frame_types = true` | 最准确，但要完整解码一遍视频，长文件明显变慢 |
| 仅关键帧 | 上面两者都拿不到 | 只有 I 帧可信，其余计入 `unknown_count`，`frame_types_known = false` |

`frame_types_known` 只有在"已知类型帧占比 > 50%"时才为 true，UI 会据此提示重扫。

## 3. 异常识别

| 类型 | 触发条件 | 默认阈值 |
| --- | --- | --- |
| `LongGop` | GOP 时长 > `max_gop_seconds` **或** 帧数 > `max_gop_frames` | 10 s / 300 帧 |
| `IrregularKeyInterval` | 关键帧间隔 stddev/mean > `gop_irregular_ratio`（整体一条） | 0.6 |
| `PeakOvershoot` | 滑动窗口码率 > 目标峰值；连续段合并成一条 | 目标峰值 = 均值 × 2 |
| `OversizedFrame` | 单帧 > 平均帧 × `large_frame_ratio` | 8 倍 |
| `OversizedIFrame` | I 帧 > 平均 I 帧 × `i_frame_oversize_ratio` | 3 倍 |
| `SceneChangeWithoutKeyframe` | 切换点 ±`scene_key_tolerance_seconds` 内无关键帧 | 0.5 s / 强度 ≥ 0.45 |
| `SparseKeyframes` | 时长 ≥ 30 s 但关键帧 ≤ 1 个 | — |

目标峰值可手动指定（`target_peak_kbps > 0`），否则自动取 `平均码率 × auto_peak_ratio`。
每种异常最多保留 `max_anomalies_per_type`（默认 200）条，超出按"峰值/大小"排序保留最严重的。

## 4. 与场景切换的关联

场景切换来自播放过程中的逐帧检测（`SceneChangeAnalyzer`），晚于全文件扫描，
所以采用**事后关联**而不是在扫描时耦合：

```cpp
// 场景切换点拿到之后（或任何时候想刷新）
analyzer::BitrateGopAnalyzer::ApplySceneChanges(result.bitrate_gop,
                                                scene_change_records_,
                                                bitrate_gop_options_);
```

- 幂等：内部先剔除旧的 `SceneChangeWithoutKeyframe` 异常再重新匹配，重复调用不会累积。
- 不会重算码率与 GOP 统计，只刷新 `scene_matches`、`anomalies` 与 `suggestions`。
- UI 上「关联场景切换」按钮手动触发；扫描完成时若已有切换点也会自动补一次。
- 关联结果同时进入 QC 报告（规则 `video.gop.scene_without_keyframe`）。

## 5. QC 规则接入

在 `QcModels.cpp::DefaultQcRules()` 新增 6 条规则，`QcRuleEngine::CheckRule()` 按 id
展开 `BitrateGopAnalysis::anomalies`：

| 规则 id | 类别 | 严重度 |
| --- | --- | --- |
| `video.gop.long_count` | GOP | Warning |
| `video.gop.scene_without_keyframe` | GOP | Info |
| `video.gop.sparse_keyframes` | GOP | Warning |
| `video.bitrate.peak_overshoot` | Bitrate | Warning |
| `video.frame.oversized` | Video | Info |
| `video.frame.oversized_i` | Video | Info |

每条规则最多展开 50 条 issue（带 `TimeRange`），超出部分合并为一条"另有 N 条未展开"。
UI「诊断与报告」页的问题表会带上时间区间，可直接跳转。

## 6. UI 使用

侧边栏 **码率与 GOP**：

1. 选滑动窗口（0.5 / 1 / 2 / 5 秒）、目标峰值（0 = 自动）、GOP 上限（秒 / 帧）。
2. 点「开始分析」——与「诊断与报告」**共用同一次全文件扫描**，不会重复读文件。
3. 曲线上叠加：I 帧（绿点，基线）、场景切换（紫方块，基线）、异常峰值（红三角，实际码率高度）、
   目标峰值（虚线水平线）。
4. 「GOP 列表」子页**点击任意一行即跳转到该 GOP 起始位置**（`SeekRequested` 信号 → 播放器）。
5. 「异常」子页点击行同样跳转；「优化建议」子页列出聚合后的编码建议。
6. 三个子页分别提供 GOP / 码率曲线 / 异常的 CSV 导出（UTF-8 BOM）。

需要精确 I/P/B 时勾选「精确帧类型（解码，较慢）」再重新扫描。

> ⚠️ 侧边栏顺序与 `AnalysisPanel` 的 `AddPageWithScroll` 调用顺序一一对应
> （见 `MainWindow.cpp::nav_items` 上方注释）。「码率与 GOP」当前排在「场景切换」之后、
> 「诊断与报告」之前，改动页面顺序必须同步两处。

## 7. 验收测试

`tests/unit/test_bitrate_gop_analyzer.cpp`（`BUILD_TESTING=ON` 时构建）：

| 验收标准 | 用例 |
| --- | --- |
| 固定 GOP 样本输出稳定 GOP 长度 | `FixedGopProducesStableLength`：300 帧 / GOP=50 → 6 个 GOP，每个 50 帧，时长标准差 ≈ 0 |
| 超长 GOP 必须产生 warning | `LongGopTriggersWarning`：40 s GOP → ≥2 条 `LongGop` + 建议 |
| VBR 显示峰值与均值差异 | `VbrShowsPeakVsAverage`：峰均比 > 1.5，多窗口峰值随窗口变短而升高 |
| 场景切换附近无关键帧给出建议 | `SceneChangeWithoutKeyframeProducesSuggestion`：4.8 s 处无关键帧 → 1 条异常；20 s 处有 → 不报 |

其余用例覆盖：仅包级输入、`SparseKeyframes`、空输入、`Finish()` 幂等、`ApplySceneChanges` 幂等。

本地快速验证（不依赖 gtest / CMake）：

```bash
# 用 cl 独立编译冒烟程序（放项目外目录，避免被 CMake GLOB 收进主目标）
cl /std:c++17 /EHsc /utf-8 /I <项目根> smoke.cpp \
   core/analyzer/BitrateGopAnalyzer.cpp core/model/BitratePoint.cpp core/model/GopInfo.cpp
```
