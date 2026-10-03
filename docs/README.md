# VideoEye 文档索引

`docs/` 自 2026-10-04 起整体纳入版本控制。此前只有 `ARCHITECTURE.md` 一篇被放行，
其余 13 篇都在 `.gitignore` 里 —— 结果是文档和代码不在同一次提交里，PR 评审看不到它，
失同步只能靠人记得同步。这次复核时就发现 7 处引用指向了早已不存在的源码文件。

## 先读哪一篇

- **要改代码** → 先读 [`ARCHITECTURE.md`](ARCHITECTURE.md)。它是分层与依赖边界的唯一
  权威说明，§7「新增代码放哪」和 §7.1「加一个分析维度要动哪些文件」是入口。
- **要看某个分析维度怎么实现的** → 在下面「按分析维度」里找对应那篇。
- **想知道这个项目踩过哪些坑** → `audit/` 下两篇。

## 架构（随代码演进，保持更新）

| 文档 | 内容 |
|------|------|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 分层架构（9 层）、依赖方向与强制手段、Qt/domain 边界、扩展成本清单。**改分层前必读** |

## 按分析维度

每篇对应一个分析器族，讲「为什么这样设计」而不是 API 列表 —— 接口本身看头文件。

| 文档 | 对应模块 |
|------|----------|
| [`BITSTREAM_ANALYSIS.md`](BITSTREAM_ANALYSIS.md) | 编码码流解析：H.264 / HEVC / AV1 的 SPS-PPS-VPS、extradata 与 OBU |
| [`MP4_SAMPLE_TABLE.md`](MP4_SAMPLE_TABLE.md) | MP4 / fMP4 容器一致性校验（sample table） |
| [`BITRATE_GOP_ANALYSIS.md`](BITRATE_GOP_ANALYSIS.md) | 码率曲线与 GOP 深度 |
| [`COLOR_HDR_ANALYSIS.md`](COLOR_HDR_ANALYSIS.md) | 色彩与 HDR 元数据（含 FFmpeg 枚举对齐的 `static_assert`） |
| [`AUDIO_QC.md`](AUDIO_QC.md) | 音频 QC：响度、真峰值、削波、静音 |
| [`VISUAL_QC.md`](VISUAL_QC.md) | 画面质量与视觉缺陷：黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 曝光 / 色偏 / 梳齿 / 黑边 |
| [`SUBTITLE_TIMECODE_AUX.md`](SUBTITLE_TIMECODE_AUX.md) | 字幕 cue、SMPTE 时码 / 章节、data 流与 SCTE-35 |
| [`HLS_DASH_SEGMENT.md`](HLS_DASH_SEGMENT.md) | HLS / DASH 清单与分片检测、多码率 ladder |

## 横切能力

| 文档 | 内容 |
|------|------|
| [`DIAGNOSTICS_QC.md`](DIAGNOSTICS_QC.md) | 诊断与 QC 报告：`DiagnosticIssue` / `QcReport` 数据契约、规则引擎、16 条默认规则 |
| [`REPORTING_BATCH_QC.md`](REPORTING_BATCH_QC.md) | 报告导出与批量 QC：模板、批处理、对比、接入 CI |
| [`FFMPEG_COMMAND_WORKBENCH.md`](FFMPEG_COMMAND_WORKBENCH.md) | FFmpeg 命令工作台：为什么它不是终端，以及这条边界带来的设计约束 |

## 审计快照（`audit/`，只读）

这两篇是**某一时刻的记录，不随代码演进**。里面的 ✅ 只代表当时修完了，不代表现在还是好的；
行号也可能已经漂移。留着它们是作为决策依据 —— 后来改代码时能看到"当初为什么这么做"。

| 文档 | 内容 |
|------|------|
| [`audit/CODE_AUDIT_2026-10-02.md`](audit/CODE_AUDIT_2026-10-02.md) | 全模块代码审查：20 条 P0 缺陷与修复记录 |
| [`audit/ARCHITECTURE_REVIEW_2026-10-04.md`](audit/ARCHITECTURE_REVIEW_2026-10-04.md) | 架构评审：10 项改进建议，含第 8 项「为什么撤回」的理由 |

## 维护约定

1. **改代码时顺手改文档**。文档进了版本控制，diff 里看得见；不同步会立刻显形。
2. **文档里写的源码路径必须是真的**。这是最容易烂的一类内容 —— 本次入库时就发现
   `core/analysis/orchestration/AnalysisTask.h` 在 6 篇文档里被引用，而该文件早已被拆成
   `AnalysisOptions.h` + `AnalysisResult.h`。搬文件时应 grep 一遍 `docs/`。
3. **新加一个分析维度**时，除 `ARCHITECTURE.md` §7.1 列的那些代码文件外，还要在
   「按分析维度」这张表里加一行。
4. **新做一轮审计**时，写进 `audit/` 并按 `审计-YYYY-MM-DD.md` 命名，不要覆盖旧的。
