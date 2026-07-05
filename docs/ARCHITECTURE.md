# VideoEye 2.0 架构设计

## 系统架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                         UI 层 (Qt6)                          │
├─────────────────────────────────────────────────────────────┤
│  MainWindow │ PlayerControls │ AnalysisPanel │ SettingsPanel │
└────────────────────────┬────────────────────────────────────┘
                         │ Qt Signals/Slots
┌────────────────────────▼────────────────────────────────────┐
│                      业务逻辑层                              │
├─────────────────────────────────────────────────────────────┤
│   MediaPlayer  │  StreamAnalyzer  │  FrameAnalyzer  │ ...   │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                      核心引擎层                              │
├─────────────────────────────────────────────────────────────┤
│  VideoDecoder  │  AudioDecoder  │  SyncManager  │  Resampler│
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                    外部依赖库                                │
├─────────────────────────────────────────────────────────────┤
│  FFmpeg 8.1 (Vulkan)│ OpenCV │ SDL2 │ Vulkan │ Bento4 │ ... │
└─────────────────────────────────────────────────────────────┘
```

## 核心模块设计

### 1. 播放器模块 (core/player)

#### MediaPlayer 类 (薄协调层)

**职责**: 协调播放流程，委托专业模块完成具体工作

**设计模式**: 状态模式、观察者模式、委托模式

```cpp
class MediaPlayer : public QObject {
    Q_OBJECT
    // 委托模块
    StreamInfoExtractor stream_info_extractor_;   // 流元数据提取
    AudioVisualizer audio_visualizer_;             // 音频可视化 (FFT)
    std::unique_ptr<VideoFrameExporter> frame_exporter_;  // 帧导出
    PlaybackClock playback_clock_;                 // 播放节奏控制
    
    // 硬件解码
    bool hw_decoding_enabled_ = true;
};
```

**线程模型**:
```
主线程 (UI)          解码线程           渲染线程
    │                   │                  │
    ├──► Play()        │                  │
    │                  ├──► ReadPacket()  │
    │                  ├──► HW/SW Decode() │
    │◄── FrameReady ───┤                  │
    │                  │                  ├──► Display()
    │                  │                  │
```

#### VideoDecoder 类 (含硬件加速)

**职责**: 视频解码，支持硬件加速与软件解码自动切换

**关键设计**:
- 使用 `avcodec_send_packet` / `avcodec_receive_frame`
- 硬件加速: 自动探测 Vulkan/VAAPI/CUDA/VDPAU/VideoToolbox/D3D11VA/DXVA2/QSV
- Vulkan 优先: 通过 VulkanContext 共享设备上下文，支持零拷贝帧访问
- HW 帧自动下载到系统内存 (兼容模式) 或保留在 GPU (零拷贝模式)
- RAII 管理 FFmpeg 资源

```cpp
class VideoDecoder {
public:
    bool Initialize(AVCodecParameters* params);           // 软件解码
    bool InitializeWithHw(AVCodecParameters* params,      // 硬件解码 (VAAPI/CUDA/...)
                          AVHWDeviceType hw_type);
    bool InitializeWithVulkanDevice(AVCodecParameters*,   // Vulkan 硬件解码
                                     AVBufferRef* vk_ctx);
    static std::vector<AVHWDeviceType> GetAvailableHwDeviceTypes();
    bool IsHardwareDecoding() const;
    bool IsCurrentFrameVulkan() const;                    // 当前帧是否 Vulkan 格式
    const AVFrame* GetLastRawFrame() const;               // 零拷贝帧访问
    bool DecodePacket(AVPacket* packet, FrameData& output);
    
private:
    bool DownloadHwFrame(AVFrame* sw_frame);  // HW 帧 → CPU
    AVBufferRef* hw_device_ctx_ = nullptr;
    AVHWDeviceType hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
    bool zero_copy_enabled_ = false;          // Vulkan 零拷贝模式
};
```

#### VulkanContext 类 (GPU 设备管理)

**职责**: 管理 Vulkan 实例/物理设备/逻辑设备，桥接 FFmpeg AVBufferRef

**关键设计**:
- 动态版本协商: `vkEnumerateInstanceVersion()` 检测驱动支持的最高 API 版本
- 优先独显 (DISCRETE_GPU)，回退任意设备
- 队列族分离: graphics / compute / transfer 独立查找
- `AVVulkanDeviceContext` 填充后通过 `av_hwdevice_ctx_init()` 交付 FFmpeg

#### VulkanRenderer 类 (GPU 渲染管线)

**职责**: 替代 `sws_scale → QImage → QLabel` CPU 渲染路径

**管线流程**:
```
AVFrame (VK/NV12) → staging upload → Y/UV textures
  → Compute Shader (YUV→RGB, BT.709)
  → Graphics Pipeline (fullscreen triangle)
  → vkQueuePresentKHR (display)
```
- 双模式: 零拷贝 Vulkan 帧 + CPU 上传 SW 帧
- MAX_FRAMES_IN_FLIGHT=2 同步 + swapchain 自适应重建

#### AudioVisualizer 类 (FFT 优化)

**职责**: 从 PCM 样本计算波形、频谱、响度

**性能优化**:
- Cooley-Tukey radix-2 FFT 替代朴素 DFT (O(N log N) vs O(N×K))
- 预计算 Hann 窗 + 旋转因子 + 位反转表
- 全局缓存 FFT 表 (线程安全)
- 单次 mono 混缩供所有计算复用
- 对数频率映射匹配听觉感知

```cpp
class AudioVisualizer {
public:
    AudioVisualizationResult Process(const int16_t* samples,
                                     int sample_count, int sample_rate, int channels) const;
private:
    struct FftTables { hann_window, twiddle_cos/sin, bit_reverse };
    static FftTables& GetTables(int fft_size);  // 缓存
    static void FftInPlace(re, im, tables);     // radix-2 蝶形运算
};
```

### 2. 分析模块 (core/analyzer)

#### StreamAnalyzer

**职责**: 分析码流统计信息

**分析内容**:
- 帧率统计
- 码率分析
- 包大小分布
- GOP 结构

```cpp
class StreamAnalyzer {
public:
    void AnalyzePacket(const AVPacket* packet);
    StreamStats GetStats() const;
    
private:
    std::vector<PacketInfo> packet_history_;
    double current_fps_;
    int current_bitrate_;
};
```

#### FrameAnalyzer

**职责**: 帧级别分析

**支持的分析**:
- 直方图分析
- 边缘检测 (Canny)
- 轮廓提取
- 2D DFT 变换

```cpp
class FrameAnalyzer {
public:
    cv::Mat ComputeHistogram(const FrameData& frame);
    cv::Mat DetectEdges(const FrameData& frame);
    cv::Mat FindContours(const FrameData& frame);
    
private:
    cv::Mat ConvertToMat(const FrameData& frame);
};
```

#### FaceDetector

**职责**: 实时人脸检测

**技术**: OpenCV Haar Cascade / DNN

```cpp
class FaceDetector {
public:
    bool Initialize(const std::string& cascade_path);
    std::vector<cv::Rect> Detect(const FrameData& frame);
    
private:
    cv::CascadeClassifier cascade_;
    // 或使用 DNN
    cv::dnn::Net dnn_model_;
};
```

#### ContainerStructureAnalyzer - 容器结构分析

**职责**: 统一解析多种视频容器格式的文件结构

**设计模式**: 策略模式 + 统一调度 + 自动回退

```
ContainerStructureAnalyzer
│
├─ FormatDetector (魔数检测 + 扩展名回退)
│
├─ MP4/MOV  → Mp4BoxAnalyzer (Bento4) → 丰富流信息 (codec/分辨率/采样率)
├─ MKV/WebM → EbmlAnalyzer          → 丰富流信息 (codec/分辨率/帧率/语言)
├─ AVI      → AviStructureAnalyzer (RIFF 递归)
├─ FLV      → FlvStructureAnalyzer (Tag 序列)
├─ MPEG-TS  → TsStructureAnalyzer (PAT/PMT)
├─ ASF/WMV  → AsfStructureAnalyzer (Object 遍历)
├─ OGG      → OggStructureAnalyzer (Page 解析)
└─ 其他/失败 → FFmpeg AVFormatContext (通用 metadata) ← 自动回退
```

**关键改进**:
- MP4 流信息: 从 Box 树提取 codec (avc1/hvc1/mp4a)、分辨率、采样率、声道数
- MKV 流信息: codec、分辨率、帧率、采样率、位深、语言、轨道名
- 格式回退: 任何专用解析器失败时自动回退 FFmpeg 通用分析
- 统一入口: MediaPlayer 仅调用 `container_analyzer_.Analyze()`，不再重复调用 Mp4BoxAnalyzer

**统一数据流**:

```
MediaPlayer::Open()
  → ContainerStructureAnalyzer::Analyze(path)
    → FormatDetector::Detect() → ContainerFormat
    → 分发到对应解析器
    → 统一映射为 ContainerStructureResult
    → emit ContainerStructureReady(result)
    → AnalysisPanel::OnContainerStructureReady()
      → 动态标题 ("文件结构 - MP4" / "文件结构 - AVI")
      → 通用结构树 + 流信息 + 元数据
      → MP4/MKV 额外显示详细表格 (QStackedWidget)
```

**统一模型** (`ContainerStructureInfo.h`):
- `ContainerElement` — 通用树节点，所有格式共用
- `ContainerStreamInfo` — 流信息
- `ContainerStructureResult` — 统一结果，保留 MP4/EBML 详细结果

### 3. 数据模型 (core/model)

#### 设计理念

- 纯数据结构,无业务逻辑
- 支持序列化
- 线程安全 (不可变对象)

```cpp
struct StreamInfo {
    std::string filename;
    std::string format_name;
    // ... 其他字段
    
    std::string ToString() const;
    nlohmann::json ToJson() const;
    static StreamInfo FromJson(const nlohmann::json& json);
};
```

### 4. UI 模块 (ui)

#### MainWindow

**职责**: 主窗口容器,协调各子模块

**组件**:
- VideoWidget: 视频显示
- ControlPanel: 播放控制
- InfoPanel: 信息显示
- AnalysisPanel: 分析结果

#### 信号槽连接

```cpp
// 播放器 -> UI
connect(player_, &MediaPlayer::FrameReady,
        video_widget_, &VideoWidget::UpdateFrame);

connect(player_, &MediaPlayer::PositionChanged,
        this, &MainWindow::UpdateProgress);

// UI -> 播放器
connect(play_button_, &QPushButton::clicked,
        player_, &MediaPlayer::Play);
```

## 数据流设计

### 播放流程

```
1. 用户打开文件
   │
   ▼
2. MainWindow::OnOpenFile()
   │
   ▼
3. MediaPlayer::Open(url)
   ├─ avformat_open_input()
   ├─ avformat_find_stream_info()
   ├─ 查找音视频流
   └─ 初始化解码器
   │
   ▼
4. 用户点击播放
   │
   ▼
5. MediaPlayer::Play()
   └─ 启动解码线程
      │
      ▼
   6. DecodeThread()
      ├─ av_read_frame() 读取包
      ├─ VideoDecoder::DecodePacket()
      ├─ 发送 FrameReady 信号
      └─ 更新播放进度
         │
         ▼
      7. VideoWidget::UpdateFrame()
         └─ 显示视频帧
```

### 分析流程

```
1. 用户触发分析
   │
   ▼
2. MainWindow::OnStreamAnalysis()
   │
   ▼
3. StreamAnalyzer::AnalyzePacket()
   ├─ 收集包信息
   ├─ 计算统计数据
   └─ 生成报告
      │
      ▼
   4. AnalysisPanel::UpdateStats()
      └─ 显示分析结果
```

## 并发设计

### 线程模型

```
┌─ 主线程 (UI) ──────────────────────┐
│  - 处理用户输入                      │
│  - 渲染界面                          │
│  - 响应信号                          │
└────────────────────────────────────┘

┌─ 解码线程 ─────────────────────────┐
│  - 读取媒体包                        │
│  - 视频解码                          │
│  - 音频解码                          │
│  - 发送帧信号                        │
└────────────────────────────────────┘

┌─ 分析线程 (可选) ──────────────────┐
│  - 帧分析                            │
│  - 特征提取                          │
│  - 结果计算                          │
└────────────────────────────────────┘
```

### 同步机制

```cpp
class MediaPlayer {
private:
    std::atomic<PlayerState> state_;      // 无锁状态
    std::mutex mutex_;                     // 保护共享资源
    std::condition_variable cv_;          // 线程通信
    std::atomic<bool> should_stop_;       // 优雅退出
};
```

## 内存管理

### RAII 原则

```cpp
// ✅ 正确: 使用智能指针
class MediaPlayer {
    std::unique_ptr<VideoDecoder> video_decoder_;
    std::unique_ptr<AudioDecoder> audio_decoder_;
};

// ❌ 错误: 手动管理
class MediaPlayer {
    VideoDecoder* video_decoder_;  // 容易泄漏
};
```

### FFmpeg 资源管理

```cpp
class VideoDecoder {
public:
    ~VideoDecoder() {
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);  // 自动释放
        }
    }
    
private:
    AVCodecContext* codec_ctx_;
};
```

## 错误处理

### 异常策略

```cpp
// 使用返回值 + 信号
bool MediaPlayer::Open(const QString& url) {
    int ret = avformat_open_input(...);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit Error(QString("Open failed: %1").arg(errbuf));
        return false;
    }
    return true;
}
```

### 日志系统

```cpp
class Logger {
public:
    static void Info(const std::string& msg);
    static void Warning(const std::string& msg);
    static void Error(const std::string& msg);
    
private:
    static std::mutex log_mutex_;
};
```

## 扩展性设计

### 插件架构 (未来)

```cpp
class IAnalyzerPlugin {
public:
    virtual ~IAnalyzerPlugin() = default;
    virtual QString GetName() = 0;
    virtual void Analyze(const FrameData& frame) = 0;
    virtual QWidget* GetWidget() = 0;
};

// 插件管理器
class PluginManager {
public:
    void RegisterPlugin(std::unique_ptr<IAnalyzerPlugin> plugin);
    void RunAllPlugins(const FrameData& frame);
};
```

### 策略模式

```cpp
// 不同的解码策略
class IDecodeStrategy {
public:
    virtual ~IDecodeStrategy() = default;
    virtual bool Decode(AVPacket* packet, FrameData& output) = 0;
};

class SoftwareDecode : public IDecodeStrategy { ... };
class HardwareDecode : public IDecodeStrategy { ... };
```

## 性能优化

### 已实现的优化

1. **FFT 频谱计算**: Cooley-Tukey radix-2 算法替代朴素 DFT，加速约 28 倍 (512 点 FFT)
2. **预计算表**: Hann 窗、旋转因子、位反转表全局缓存，三角函数调用降为 0
3. **单次 mono 混缩**: 声道混合仅执行一次，波形/频谱/响度/峰值复用
4. **硬件解码**: 支持 VAAPI/CUDA/VideoToolbox 等，减少 CPU 负载
5. **零拷贝**: 尽量减少数据复制
6. **异步 I/O**: 非阻塞读取

```cpp
// FFT 性能对比 (512 点)
// DFT:  512 × 64 × 2 = 65,536 次 sin/cos
// FFT:  512 × 9 / 2  = 2,304 次蝶形 (查表)
// 加速比: ~28x
```

## 测试策略

### 测试框架

- **框架**: GoogleTest v1.15.2 (FetchContent 自动下载)
- **构建**: CMake `BUILD_TESTING` option 控制
- **运行**: `cd build-debug && ctest --output-on-failure`
- **规模**: 6 个测试套件，115 个测试用例

### 测试覆盖

| 测试套件 | 被测模块 | 用例数 | 说明 |
|---------|---------|--------|------|
| test_format_detector | FormatDetector | 27 | 魔数检测、扩展名推断、优先级 |
| test_config_manager | ConfigManager | 26 | 配置读写、文件加载、持久化 |
| test_report_exporter | ReportExporter | 14 | TXT/CSV/JSON/HTML 导出 |
| test_stream_analyzer | StreamAnalyzer | 17 | 帧统计、并发线程安全 |
| test_frame_data | FrameData | 15 | 数据模型、深拷贝、StreamInfo |
| test_frame_analyzer | FrameAnalyzer | 16 | 直方图、边缘检测、OpenCV 转换 |

### 单元测试示例

```cpp
TEST(FormatDetectorTest, DetectMP4ByMagic) {
    // 创建带 ftyp 头部的临时文件
    QTemporaryFile tmpFile;
    tmpFile.open();
    QByteArray header;
    header.append("\x00\x00\x00\x1C");  // box size
    header.append("ftyp");               // box type
    header.append("isom");               // major brand
    tmpFile.write(header);
    tmpFile.close();

    FormatDetector detector;
    auto result = detector.Detect(tmpFile.fileName().toStdString());
    EXPECT_EQ(result.format, ContainerFormat::MP4);
}
```

### 集成测试 (待实现)

```cpp
TEST(MediaPlayerTest, PlayLocalFile) {
    MediaPlayer player;
    
    EXPECT_TRUE(player.Open("test.mp4"));
    player.Play();
    
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    EXPECT_EQ(player.GetState(), PlayerState::Playing);
    EXPECT_GT(player.GetCurrentPosition(), 0);
}
```

## 安全考虑

1. **输入验证**: 检查所有用户输入
2. **资源限制**: 限制缓冲区大小
3. **异常处理**: 防止崩溃
4. **内存安全**: 使用智能指针

## 未来规划

### 短期 (已完成)

- [x] 完成基础播放功能
- [x] 实现流分析
- [x] 实现容器结构分析 (7 种格式 + FFmpeg 回退)
- [x] 单元测试 (6 suites, 115 cases)
- [x] MediaPlayer 拆分重构 (PlaybackClock/StreamInfoExtractor/AudioVisualizer/VideoFrameExporter)
- [x] FFT 性能优化 (Cooley-Tukey 算法，预计算表)
- [x] 硬件解码支持 (Vulkan/VAAPI/CUDA/VideoToolbox/D3D11VA 等，自动探测与回退)
- [x] Vulkan GPU 渲染管线 (YUV→RGB compute shader + 零拷贝 present)
- [x] 一键环境初始化脚本 (setup.sh)
- [x] 容器分析调度完善 (丰富流信息、格式回退、统一入口)
- [ ] 完善 UI

### 中期 (3-6个月)

- [ ] 集成测试
- [ ] 更多分析算法
- [ ] 视频帧导出支持硬件解码

### 长期 (6-12个月)

- [ ] AI 增强分析
- [ ] CI/CD 集成
- [ ] 插件系统
- [ ] 移动端支持

---

**架构持续演进中...** 🚀
