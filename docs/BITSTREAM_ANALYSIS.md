# 编码码流解析技术文档

## 1. 概述

### 1.1 专业价值

容器 metadata 有时不可信，必须解析 codec bitstream 才能确认真实编码参数。本功能通过直接解析 H.264/H.265/AV1/VVC的码流头部信息（SPS/VPS/OBU 等），提取精确的编码参数，并与容器 metadata 进行对比，发现不一致项。

### 1.2 支持的编码格式

- **H.264 (AVC)**: SPS/PPS、profile、level、VUI、HRD、SEI
- **H.265 (HEVC)**: VPS/SPS/PPS、profile tier level、VUI、HDR SEI
- **AV1**: OBU、sequence header、film grain、HDR metadata
- **VVC (H.266)**: 基本头信息（简化版）

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
    
    std::vector<HevcSeiMessage> sei_messages; // SEI 消息列表
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
    
    Av1ColorConfigInfo color_config;  // 色彩配置
    Av1FilmGrainInfo film_grain;      // Film Grain 信息
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
#include "core/analyzer/BitstreamAnalyzer.h"

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
// 在 AnalysisCoordinator 中
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

## 10. 参考资料

- ITU-T H.264 (AVC): ISO/IEC 14496-10
- ITU-T H.265 (HEVC): ISO/IEC 23008-2
- AOMedia AV1 Specification: https://aomedia.googlesource.com/media-video/av1/
- MPEG-VVC (H.266): ISO/IEC 23090-3
- SMPTE ST 2086: Mastering Display Color Volume
- CTA-861.3: Electronic Standard for the Light Level of Television Displays

## 附录：代码文件清单

### 文件清单

1. `utils/BitReader.h/.cpp` - 位流读取器
2. `utils/ExtradataParser.h` - 封装格式识别接口
3. `core/model/BitstreamInfo.h` - 数据模型
4. `core/analyzer/H264BitstreamParser.h/.cpp` - H.264 解析器
5. `core/analyzer/HevcBitstreamParser.h/.cpp` - H.265 解析器
6. `core/analyzer/BitstreamAnalyzer.h/.cpp` - 统一接口
7. `core/analyzer/AnalysisTask.h` - 扩展（已添加 bitstream_analysis 字段）
8. `ui/bitstream_panel/BitstreamPanel.h` - UI 组件框架

9. `utils/ExtradataParser.cpp` - 封装格式识别实现（avcC / hvcC / av1C）
10. `core/analyzer/Av1BitstreamParser.h/.cpp` - AV1 解析器
11. `core/analyzer/VvcBitstreamParser.h` - VVC 解析器（仅基础头信息，简化版）
12. `ui/bitstream_panel/BitstreamPanel.cpp` - UI 实现
13. `tests/unit/test_h264_bitstream_parser.cpp`、`tests/unit/test_hevc_bitstream_parser.cpp` - 单元测试

---

**版本**: 1.0  
**最后更新**: 2026-09-21  
**维护者**: VideoEye Team
