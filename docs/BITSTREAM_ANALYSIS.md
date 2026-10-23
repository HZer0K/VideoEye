# 编码码流解析技术文档

## 1. 概述

### 1.1 专业价值

容器 metadata 有时不可信，必须结合 codec bitstream 才能确认真实编码参数。本功能解析 H.264/H.265/AV1/VVC 的码流头部信息（SPS/VPS/OBU 等），并按能力状态决定是否与容器 metadata 做严格对比；标记为 `Partial` 的字段只供诊断参考，不作为正式结论。

### 1.2 支持的编码格式

口径说明：**只写解析器真正落地成字段的东西**。HDR 静态元数据（ST 2086 / MaxCLL-MaxFALL）
与 Dolby Vision 配置走的是 FFmpeg 的 side data（见 `COLOR_HDR_ANALYSIS.md`），
不是本模块从 SEI 里解出来的 —— 早期文档把这两条写在一起，会让排期误判工作量。

- **H.264 (AVC)**: SPS（profile / level / 色度 / 位深 / cropping / VUI 色彩与时基）、PPS；
  **HRD 只做按位跳过**（保证 VUI 后续字段不错位，不落地数值），**不解析 SEI**
- **H.265 (HEVC)**: VPS / SPS / PPS、profile tier level、VUI、conformance window；
  **不解析 SEI**（详见 1.5）
- **AV1**: OBU、sequence header、color_config、`film_grain_params_present` 标志
  （只取这个布尔标志：film grain 是否启用；**不解析** film grain 的具体参数，
  详见 1.5）
- **VVC (H.266，实验性/部分支持)**: VPS/SPS/PPS、profile tier level、VUI；PPS 为浅解析，字段仅供参考

### 1.3 参数集从哪来：extradata 与带内

参数集有两个来源，结果里的 `parameter_set_source` 会写明，消费方不该混着看：

| 来源 | 适用封装 | 做法 | 成本 |
|------|---------|------|------|
| `extradata` | MP4/MOV、MKV/WebM（avcC / hvcC / av1C / vvcC 或 AnnexB extradata） | 直接读 `AVCodecParameters::extradata` | KB 级，可忽略 |
| `in_band` | **MPEG-TS / 裸流 / fMP4 媒体分片**（extradata 恒为空） | 在扫描前 8 MiB / 4000 个视频包里找 SPS/PPS/VPS/序列头，命中即解析并停手 | 一次 O(n) 起始码扫描，命中前不解语法 |

带内路径的判据与上限都在 `core/analysis/codec/InBandParameterSets.h`：

- AnnexB（H.264/HEVC/VVC）增量扫起始码 + 按 codec 读 NAL 类型判断"参数集出现了"，
  只在出现时才调一次 `BitstreamAnalyzer::AnalyzeInBand()`；扫描位置带 5 字节重叠，
  起始码/NAL header 被包边界切开也能认出来。
- AV1 没有起始码，按 1/2/4/8… 的对数间隔试解（序列头几乎总在最前面几个包）。
- 放弃时会给出可读原因（"已扫满 N 个包仍未见参数集"），并写进日志；
  典型场景是"录播从 GOP 中间开始、且编码器只在流开头写了一次参数集"。
- 这条路径**只在没有 extradata 时才启用**：extradata 有值就是权威来源，不做二次探测。

### 1.4 明确不做：SEI 消息与 AV1 film grain 参数

这两块曾经"定义了结构体但没有任何产出"，2026-10-18 已删除。留着它们的代价不是
代码行数，而是**假象**：看代码的人会以为能力已经具备，实际结论恒为空。

| 已删除 | 原位置 | 为什么不留 |
|--------|--------|-----------|
| `HevcBitstreamParser::ParseSeiMessages()` + `HevcSeiMessage` + `HevcVpsInfo::sei_messages` | `core/analysis/codec/HevcBitstreamParser.{h,cpp}`、`core/domain/model/HevcBitstreamInfo.h` | 全项目**零调用**，`sei_messages` 恒空。HDR 静态/动态元数据（ST 2086、MaxCLL/MaxFALL、HDR10+、Dolby Vision）按 `README.md` 的"不自研"清单走 **FFmpeg 的 side data**（解码首帧兜底），不从 SEI 里解。删它与架构声明一致，不是丢能力 |
| `Av1FilmGrainInfo`（21 个字段） + `Av1SequenceHeaderInfo::film_grain` | `core/domain/model/Av1BitstreamInfo.h` | 结构体随 seq header 一起被实例化，但**21 个字段从未被任何代码写入**，永远全零，且没有任何报告/UI 出口 |

保留的是 `Av1SequenceHeaderInfo::film_grain_params_present`（布尔）：它在
`Av1BitstreamParser.cpp` 里真的从 sequence header 读出，也在码流面板里展示 ——
回答的是"这条流有没有开 film grain"，够用且可验证。若将来要展示具体参数
（AR 系数、grain seed 等），应连同**解析代码 + 报告/UI 出口 + 用例**一起加，
不要只把结构体放回来。

## 2. 技术原理

### 2.1 NAL 单元与 OBU

#### H.264/H.265 - NAL Unit

NAL (Network Abstraction Layer) 单元是 H.264/H.265 的基本数据单元：

```
+------------------+
| NAL Header (1B)  | <- 包含 nal_unit_type
+------------------+
| NAL Payload      | <- SPS/PPS/SEI 等数据
+------------------+
```

**NAL 单元类型**:
- Type 7: SPS (Sequence Parameter Set)
- Type 8: PPS (Picture Parameter Set)
- Type 32: VPS (Video Parameter Set, H.265)
- Type 39: SEI (Supplemental Enhancement Information)

#### AV1 - OBU

OBU (Obuservable Unit) 是 AV1 的基本数据单元：

```
+------------------+
| OBU Header       | <- 包含 obu_type
+------------------+
| OBU Payload      | <- Sequence Header 等
+------------------+
```

**OBU 类型**:
- Type 0: Sequence Header
- Type 4: Frame Header
- Type 5: Frame
- Type 12: Film Grain

### 2.2 封装格式

#### Annex B (起始码格式)

使用起始码分隔 NAL 单元：
```
00 00 00 01 [NAL1] 00 00 00 01 [NAL2] ...
或
00 00 01 [NAL1] 00 00 01 [NAL2] ...
```

#### Length-Prefix (长度前缀格式)

每个 NAL 单元前有长度前缀：
```
[Length: 1/2/4 bytes][NAL1][Length: 1/2/4 bytes][NAL2]...
```

#### Configuration Record (配置记录)

MP4 容器中的 extradata 格式：
- **avcC**: H.264 配置记录
- **hvcC**: H.265 配置记录
- **av1C**: AV1 配置记录

### 2.3 位流解析

所有编码均使用**小端序 (Little-Endian)** 读取位流：

```cpp
// 示例：读取 3 位
uint32_t value = reader.ReadBits(3);

// 示例：Exp-Golomb 解码 (UE)
uint32_t ue_value = reader.ReadUWord();

// 示例：对齐到字节边界
reader.AlignToByte();
```

### 2.4 ue(v) 不能直接当循环上界

ue(v) 的合法编码能解出 2^32 量级的数，而解析器读到的这类字段经常正好是**循环次数**
（`num_tile_columns_minus1`、`num_negative_pics`、`cpb_cnt_minus1`…）。
拿裸 `ReadUE()` 的结果当上界，畸形码流就能把一次解析变成数十亿次迭代：
CI 的 libFuzzer 入口（`tests/fuzz/`）在 17 字节的 HEVC PPS 上就是这么 10 秒超时的。

规矩：**凡是 ue(v) 决定的循环次数，一律走 `ReadUEBounded(规范上限)`**
（超界置错误、整份参数集判无效）或 `ReadUEClamped`（诊断场景，只夹断不判错），
上限写成 `kMax*` 常量放文件顶部。H.264 / VVC 一直是这么写的；HEVC 的
PPS tile 数与短期参考帧集的 `num_negative/positive_pics` 是漏网的两处，
2026-10-23 补上（回归用例 `CorpusRobustnessTest.HevcPpsAbsurdTileCountIsRejected`）。

### 2.5 ue(v) 也不能直接进算术

§2.4 说的是"别拿它当循环上界"，其实还有第二种更隐蔽的用法：**拿它做加减乘和移位**。
`ReadUE()` 的上界是 `kMaxVlcBits = 32` 位能编码出的值（可到 2^32-2），`ReadSE()` 到
±2^31 —— 这些值一旦参与 `int` 运算就是有符号溢出，**是 UB，不是"回绕成一个大数"**：
优化器可以基于"不会溢出"做任何重排，事后判断结果正负完全兜不住。

2026-10 的 fuzz 一轮（`-fno-sanitize-recover` 打开之前它是静默的）在四条路径上抓到：

| 位置 | 表达式 | 触发值 |
|---|---|---|
| `H264SpsInfo::height()` | `(crop_top + crop_bottom) * CropUnitY()` | `2147479088 * 2` |
| `H264BitstreamParser` SPS 校验 | `pic_width_in_mbs_minus1 + 1` | `2147483647 + 1` |
| `H264BitstreamParser::SkipScalingList` | `last_scale + delta_scale + 256` | `251 + 2147483520` |
| `HevcBitstreamParser` CTU 反推 | `1 << (... + 3 + log2_diff)` | 移位量 `13563075` |

**规矩（三条，缺一条就会重新长回来）：**

1. **换算用 int64，出口夹断。** 像素换算（`×16`、`× CropUnit`）一律 `int64_t` 中间量，
   最后 `ClampDimension()` 夹到 `[0, INT_MAX]`，负值/超界统一返回 **0 = 未知**
   （调用方本来就只在 `>0` 时采信）。
2. **校验语句自己不能溢出。** `if (x + 1 > 200)` 这种写法在 `x = INT_MAX` 时，判的
   那一刻就已经 UB 了 —— 必须先把 `x` 转 `int64_t` 再比（见上面的 SPS 尺寸校验）。
3. **`1 << x` 必须先验 `x`。** 移位量越界同样是 UB，且 `x` 越界时结果可以"看起来"
   合理。`VvcSpsInfo::Pow2Checked()` 就是为此存在的：先挡负数与上界，再移位。

对应的字段级上界（规范给的，别自己发明）：
H.264 `delta_scale ∈ [-128,127]`、crop offset 小于图像尺寸；
HEVC `log2_min_luma_coding_block_size_minus3` 与 `log2_diff_max_min_luma_coding_block_size ∈ [0,3]`；
VVC `sps_log2_min_luma_coding_block_size_minus2 ∈ [0,4]`。
超界一律用 `ReadUEBounded` 置错误 → 整份参数集判无效，**不要夹断**（夹断会留下一份
字段全面错位却 `present=true` 的参数集，比请求失败难查得多）。

## 3. 核心数据结构

### 3.1 H.264 SPS 关键参数
```cpp
struct H264SpsInfo {
    int profile_idc;              // 7=High, 8=High 10, 9=High 4:2:2, 10=High 4:4:4
    int level_idc;                // 31=3.1, 32=3.2, ..., 51=5.1
    int chroma_format_idc;        // 0=4:0:0, 1=4:2:0, 2=4:2:2, 3=4:4:4
    int bit_depth_luma_minus8;    // actual = value + 8
    int bit_depth_chroma_minus8;  // actual = value + 8
    int max_num_ref_frames;       // 最大参考帧数
    int pic_width_in_mbs_minus1;  // width = (value + 1) * 16
    int pic_height_in_mbs_minus1; // height 计算类似
    
    H264VuiInfo vui;              // VUI 信息（timing/color/HRD）
};
```

### 3.2 H.265 SPS 关键参数

```cpp
struct HevcSpsInfo {
    int chroma_format_idc;        // 0=4:0:0, 1=4:2:0, 2=4:2:2, 3=4:4:4
    int pic_width_in_ctu_minus1;  // CTU size default 64, width = (value + 1) * 64
    int pic_height_in_ctu_minus1;
    int bit_depth_luma_minus8;    // actual = value + 8
    int bit_depth_chroma_minus8;  // actual = value + 8
};
```

### 3.3 AV1 Sequence Header 关键参数

```cpp
struct Av1SequenceHeaderInfo {
    int profile;                  // 0=Profile 0, 1=Profile 1, 2=Profile 2
    int level;                    // 0..63 (actual level = value / 2)
    int frame_width_minus_1;      // width = value + 1
    int frame_height_minus_1;     // height = value + 1
    int bit_depth_minus_8;        // actual bit depth = value + 8
    
    Av1ColorConfigInfo color_config;      // 色彩配置
    bool film_grain_params_present;       // 是否启用 film grain（只取标志，不解析参数）
};
```

## 4. Container vs Bitstream 对比

### 4.1 常见不一致场景

| 字段 | 容器可能值 | 码流真实值 | 影响 |
|------|-----------|-----------|------|
| Color Primaries | BT.709 | BT.2020 | 颜色显示偏色 |
| Bit Depth | 8bit | 10bit | HDR 内容被降级为 SDR |
| Width/Height | 1920x1080 | 1920x1076 | 因 frame cropping 导致差异 |
| Profile | High | Main 10 | 平台不支持该 profile |

### 4.2 对比逻辑

```cpp
// 1. 从容器获取 metadata
ContainerMetadata container_meta;
container_meta.width = stream->width;
container_meta.color_primaries = stream->color_space;
// ...

// 2. 从 extradata 解析码流
BitstreamAnalyzer analyzer;
analyzer.SetContainerMetadata(container_meta);
auto result = analyzer.Analyze(extradata, size, codec_id);

// 3. 自动检测不一致
for (const auto& inconsistency : result.inconsistencies) {
    printf("Warning: %s\n", inconsistency.description.c_str());
    printf("  Container: %s\n", inconsistency.container_value.c_str());
    printf("  Bitstream: %s\n", inconsistency.bitstream_value.c_str());
}
```

## 5. 使用示例

### 5.1 基本用法

```cpp
#include "core/analysis/codec/BitstreamAnalyzer.h"

// 准备数据
const uint8_t* extradata = av_stream->codecpar->extradata;
size_t size = av_stream->codecpar->extradata_size;
int codec_id = av_stream->codecpar->codec_id;

// 创建 analyzer 并设置容器 metadata
videoeye::analyzer::BitstreamAnalyzer analyzer;

videoeye::analyzer::ContainerMetadata container_meta;
container_meta.codec_name = "h264";
container_meta.width = 1920;
container_meta.height = 1080;
container_meta.bit_depth = 8;
container_meta.color_primaries = 1; // BT.709
analyzer.SetContainerMetadata(container_meta);

// 执行分析
auto result = analyzer.Analyze(extradata, size, codec_id);

// 获取 H.264 结果
if (result.has_h264 && result.h264_sps.present) {
    auto& sps = result.h264_sps;
    
    printf("Profile: %s\n", sps.ProfileName().c_str());
    printf("Level: %s\n", sps.LevelVersion().c_str());
    printf("Resolution: %dx%d\n", sps.width(), sps.height());
    printf("Bit Depth: %d-bit\n", sps.BitDepthLuma());
    printf("Chroma: %s\n", 
           sps.chroma_format_idc == 1 ? "4:2:0" : 
           sps.chroma_format_idc == 2 ? "4:2:2" : "4:4:4");
    
    // VUI 信息
    if (sps.vui.present) {
        printf("Time Scale: %d\n", sps.vui.time_scale);
        printf("Fixed Frame Rate: %s\n", 
               sps.vui.fixed_frame_rate ? "Yes" : "No");
        
        // HRD 信息
        if (sps.vui.hrd_present) {
            printf("CPB Count: %d\n", sps.vui.cpb_cnt);
        }
    }
}

// 检查不一致警告
if (!result.inconsistencies.empty()) {
    printf("\n⚠️ 检测到 %zu 处不一致:\n\n", result.inconsistencies.size());
    
    for (const auto& inc : result.inconsistencies) {
        printf("[%s] %s\n", 
               inc.severity.c_str(), 
               inc.description.c_str());
        printf("  容器：%s\n", inc.container_value.c_str());
        printf("  码流：%s\n", inc.bitstream_value.c_str());
        if (!inc.suggestion.empty()) {
            printf("  建议：%s\n", inc.suggestion.c_str());
        }
        printf("\n");
    }
}

// 导出 JSON
std::string json = result.ToJson();
```

### 5.2 集成到 AnalysisResult

```cpp
// 在 QtAnalysisController 中
void Run() {
    // ... 其他分析 ...
    
    // 码流解析（独立触发，不自动运行）
    if (user_requested_bitstream_analysis) {
        videoeye::analyzer::BitstreamAnalyzer analyzer;
        
        // 设置容器 metadata
        videoeye::analyzer::ContainerMetadata meta;
        meta.width = result.streams[0].width;
        meta.height = result.streams[0].height;
        // ...
        analyzer.SetContainerMetadata(meta);
        
        // 解析
        auto bs_result = analyzer.Analyze(extradata, size, codec_id);
        
        // 存入 AnalysisResult
        result.bitstream_analysis = bs_result;
        result.bitstream_analyzed = true;
    }
}
```

## 6. UI 呈现

### 6.1 布局设计

```
┌─────────────────────────────────────────────────┐
│  码流分析                                         │
├─────────────────────────────────────────────────┤
│  [刷新] [复制 JSON]                               │
├──────────────┬────────────────┬────────────────┤
│  结构树      │  参数对比表    │  不一致警告    │
│              │                │                │
│  📁 H.264    │  字段      │ 容器  │ 码流   │  ⚠️ Warning: Color Primaries │
│  ├── SPS    │  ├───────────────────────────┤  │  容器：BT.709 (1)        │
│  │  ├Profile│  Profile  │ High  │ High 10  │  │  码流：BT.2020 (9)       │
│  │  ├Level  │  Level    │ 3.1   │ 5.1    │  │  影响：颜色显示可能偏色    │
│  │  └Width  │  Width    │ 1920  │ 1920   │  │                        │
│  ├── PPS    │  Chroma   │ 4:2:0 │ 4:2:0  │  │  💡 建议：检查编码参数   │
│  └VUI       │  BitDepth │ 8bit  │ 10bit  │  │                        │
│              │  └───────────────────────────┤  └──────────────────────┘
│              │                                │
└──────────────┴────────────────┴────────────────┘
```

### 6.2 三列对比说明

| 列名 | 含义 | 数据来源 |
|------|------|---------|
| 字段 | 参数名称 | 固定 |
| 容器 | 容器 metadata 中的值 | AVStream/AVCodecParameters |
| 码流 | 从 extradata 解析的值 | SPS/VPS/OBU 解析 |

**高亮规则**:
- 两列值不同 → 红色背景
- 两列值相同 → 绿色背景
- 任一为空 → 灰色背景

## 7. 测试验收标准

### 7.1 单元测试

```cpp
// test_h264_bitstream_parser.cpp
TEST(H264BitstreamParserTest, ParseHighProfileSPS) {
    // 构造 H.264 High Profile SPS 样本
    uint8_t sps_data[] = { /* ... */ };
    
    auto result = H264BitstreamParser::ParseFromNalUnit(sps_data, sizeof(sps_data));
    
    EXPECT_TRUE(result.present);
    EXPECT_EQ(result.profile_idc, 100);  // High Profile
    EXPECT_EQ(result.level_idc, 41);     // Level 4.1
    EXPECT_EQ(result.chroma_format_idc, 1); // 4:2:0
    EXPECT_EQ(result.bit_depth_luma_minus8, 2); // 10-bit
}
```

### 7.2 验收测试样本

需要准备以下测试文件：

1. **H.264 avcC 封装样本**
   - MP4 容器，H.264 High Profile 4:2:0 10-bit
   - 验证 avcC 配置记录解析

2. **H.264 Annex B 样本**
   - TS/M2TS 容器，Annex B 封装
   - 验证起始码识别和 NAL 提取

3. **H.265 hvcC 样本**
   - MP4 容器，HEVC Main 10 Profile
   - 验证 VPS/SPS/PPS 解析和 PTI 提取

4. **AV1 序列样本**
   - WebM 容器，AV1 编码
   - 验证 OBU 解析和 Sequence Header 提取

5. **不一致性测试样本**
   - 容器标 BT.709 但码流标 BT.2020 的文件
   - 容器标 8bit 但码流标 10bit 的文件
   - 验证不一致警告正确生成

### 7.3 验收检查清单

- [ ] H.264 avcC 封装能正确解析 profile/level/chroma/bit_depth
- [ ] H.264 Annex B 封装能正确提取 NAL 单元
- [ ] H.265 hvcC 封装能解析 VPS/SPS/PPS 和 profile tier level
- [ ] AV1 sequence header 能输出 bit depth/color config
- [ ] 容器与码流 metadata 不一致时输出 warning
- [ ] Tree view 正确展示层级结构
- [ ] Table view 正确显示三列对比
- [ ] 不一致项用红色高亮
- [ ] JSON 导出功能正常
- [ ] 无内存泄漏

## 8. 性能考虑

### 8.1 解析耗时

| 操作 | 耗时 | 备注 |
|------|------|------|
| H.264 SPS 解析 | < 1ms | 单线程 |
| H.265 SPS 解析 | < 1ms | 单线程 |
| AV1 OBU 解析 | < 2ms | 单线程 |
| Container 对比 | < 0.5ms | 纯内存操作 |

### 8.2 优化建议

1. **后台线程执行**: 避免阻塞 UI 主线程
2. **结果缓存**: 同一文件多次访问时使用缓存结果
3. **按需解析**: 只解析用户请求的编码类型

## 9. 常见问题

### Q1: 为什么容器和码流的 metadata 会不一致？

**A**: 可能有以下原因：
1. 编码器 bug 未正确写入容器 metadata
2. 容器编辑工具修改了视频但未更新 metadata
3. 转码过程中 metadata 丢失或错误
4. 播放器/渲染器优先使用码流信息

### Q2: 如何修复不一致问题？

**A**: 
1. **轻度不一致** (如 color primaries): 通常不影响播放，可忽略
2. **严重不一致** (如 bit depth): 可能导致 HDR 降级，需要重新编码
3. **尺寸不一致**: 检查是否有 frame cropping，可能需要调整显示区域

### Q3: 是否所有编码都需要解析？

**A**: 不需要。建议在以下情况触发：
- 用户主动点击"码流"tab
- 检测到 HDR 内容需要精确元数据
- QC 报告需要详细编码参数

## 10. 当前接入状态与已知限制

全文件诊断由 `AnalysisOptions::analyze_bitstream`（默认开）控制，在
`AnalysisPipeline::PrepareBitstream()` 里对第一条视频流的
`AVCodecParameters::extradata` 调用 `BitstreamAnalyzer::Analyze()`，结果放进
`AnalysisResult::bitstream_analysis`（`bitstream_analyzed=true` 表示跑过）。
这一步不解码，只读 KB 级 extradata。

**extradata 为空时转带内路径**（2026-10 补齐，见 §1.3）：`PrepareBitstream()` 只登记
流号，真正的攒包与解析在 `AnalysisPipeline::OnPacket()` 里逐包推进
（`InBandParameterSetProbe::Feed()`）。成功时结果与 extradata 路径同构，只是
`parameter_set_source == "in_band"`；失败（扫满包数/字节上限）会写日志并保持
`bitstream_analyzed=false`，UI 不显示码流页，行为与以前一致。

这条路径解决的问题：MPEG-TS、裸流（AnnexB ES）、fMP4 媒体分片的 extradata **恒为空**，
在本次改动前它们一律落 `NoExtradata`，码流分析整块不可用 —— 而这三种封装恰好是
直播与 HLS 点播分片的主战场。

分发用的是 FFmpeg 的 `AVCodecID`（`AV_CODEC_ID_H264` / `HEVC` / `AV1` / `VVC`），
不是自定义编号 —— 传错就静默落到 default 分支的坑已经修掉。

| 编码 | 参数集 | 正式能力口径 |
|------|--------|--------------|
| H.264 | SPS + PPS | avcC / AnnexB；Baseline/Main/High/High 10、4:2:0、8/10bit 组合已有规范回归与真实 x264 样本，判 `Supported`；其它组合判 `Partial` |
| HEVC | VPS + SPS + PPS | hvcC / AnnexB；Main/Main 10/Main Rext、4:2:0、8/10bit 组合已有规范回归与真实 x265 样本，判 `Supported`；仅 VPS 或其它组合判 `Partial` |
| AV1 | Sequence Header OBU | 走 av1C；只有 av1C（无序列头 OBU）时给位深/采样结构，判 `Partial` |
| VVC | VPS + SPS + PPS | 实验性/部分支持；即使解析出 SPS 仍判 `Partial`，不参与严格容器比对 |

**能力状态**：结果里的 `parse_outcome`（`NoExtradata` / `UnsupportedCodec` /
`TypeOnly` / `ParseFailed` / `FullParse`）与 `capability`（`Supported` / `Partial` /
`Failed` / `Unavailable`）共同描述“跑到什么程度、结果可信到什么程度”，UI 与报告
必须同时消费它们，不能只看 `analyzed`。`kCompareParameterSetFields` 已打开：
只有 `capability == Supported` 的编码才参与“码流层 vs 容器层”的宽高/位深/色彩比对
（几何差 `kGeometryTolerancePixels = 8` 像素以内记为 info）。`Partial` 字段仍可在
结构树中查看，但摘要明确标“参考”，对比表不显示“一致/不一致”结论。

**已知限制**：VVC 的 PPS 只读到 `subpic_id_mapping`，不解析 tile / slice 划分；
当前真实样本只覆盖 libvvenc 640x360 / 10bit / 4:2:0 的有限组合。VVC 的宽高、位深、
色彩字段均只供参考，正式发布不得宣传为完整支持。H.264/HEVC 的非常用 Profile、
4:2:2/4:4:4、场编码和多子层组合仍需继续扩充真实样本矩阵。

当前已入库的真实编码器回归样本：

| 样本 | 编码器与覆盖点 | FFprobe 参考结果 |
|------|----------------|------------------|
| `tests/corpus/codec/real_x264_192x108_hrd.h264` | FFmpeg 6.1 + libx264；Annex B、裁剪、VUI、HRD | Constrained Baseline / 192x108 / 8bit / BT.709 |
| `tests/corpus/codec/real_x265_rext_10bit_192x108_hrd.hevc` | FFmpeg 6.1 + libx265；Annex B、Main Rext、10bit、VUI、HRD | Rext / 192x108 / 10bit / BT.709 |

合成逐位用例负责锁定语法分支，以上真实样本负责防止“生成器和解析器共享同一错误理解”。
待补矩阵包括 4:2:2/4:4:4、场编码、多子层、广播 HRD、不同编码器与异常截断样本。

相关单测：`tests/unit/test_h264_bitstream_parser.cpp`、
`tests/unit/test_hevc_bitstream_parser.cpp`、`tests/unit/test_av1_bitstream_parser.cpp`、
`tests/unit/test_vvc_bitstream_parser.cpp`、`tests/unit/test_bitstream_analyzer.cpp`
（`BUILD_TESTING=ON` 时构建）。

## 11. 参考资料

- ITU-T H.264 (AVC): ISO/IEC 14496-10
- ITU-T H.265 (HEVC): ISO/IEC 23008-2
- AOMedia AV1 Specification: https://aomedia.googlesource.com/media-video/av1/
- MPEG-VVC (H.266): ISO/IEC 23090-3
- SMPTE ST 2086: Mastering Display Color Volume
- CTA-861.3: Electronic Standard for the Light Level of Television Displays

## 附录：代码文件清单

### 文件清单

1. `core/media/codec/BitReader.h/.cpp` - 位流读取器
2. `core/media/codec/ExtradataParser.h` - 封装格式识别接口
3. `core/domain/model/BitstreamInfo.h` - 数据模型（umbrella 聚合；按 codec 拆有 `H264BitstreamInfo.h` / `HevcBitstreamInfo.h` / `Av1BitstreamInfo.h` / `VvcBitstreamInfo.h` 四个子头）
4. `core/analysis/codec/H264BitstreamParser.h/.cpp` - H.264 解析器
5. `core/analysis/codec/HevcBitstreamParser.h/.cpp` - H.265 解析器
6. `core/analysis/codec/BitstreamAnalyzer.h/.cpp` - 统一接口
7. `core/analysis/AnalysisOptions.h` + `core/domain/model/AnalysisResult.h` - 扩展（已添加 bitstream_analysis 字段）
8. `ui/bitstream_panel/BitstreamPanel.h` - UI 组件框架

9. `core/media/codec/ExtradataParser.cpp` - 封装格式识别实现（avcC / hvcC / av1C）
10. `core/analysis/codec/Av1BitstreamParser.h/.cpp` - AV1 解析器
11. `core/analysis/codec/VvcBitstreamParser.h/.cpp` - VVC 解析器（VPS/SPS/PPS；PPS 为浅解析，止于 subpic id mapping）
12. `ui/bitstream_panel/BitstreamPanel.cpp` - UI 实现
13. `tests/unit/test_h264_bitstream_parser.cpp`、`tests/unit/test_hevc_bitstream_parser.cpp` - 单元测试

---

**版本**: 1.0  
**最后更新**: 2026-09-21  
**维护者**: VideoEye Team
