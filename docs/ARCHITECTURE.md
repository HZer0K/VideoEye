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
│      FFmpeg      │      OpenCV      │      SDL2      │ ...  │
└─────────────────────────────────────────────────────────────┘
```

## 核心模块设计

### 1. 播放器模块 (core/player)

#### MediaPlayer 类

**职责**: 协调音视频播放流程,管理状态

**设计模式**: 状态模式、观察者模式

```cpp
class MediaPlayer : public QObject {
    Q_OBJECT
    
signals:
    void StateChanged(PlayerState state);
    void FrameReady(const QImage& frame);
    void PositionChanged(int position, int duration);
    
public slots:
    void Open(const QString& url);
    void Play();
    void Pause();
    void Stop();
};
```

**线程模型**:
```
主线程 (UI)          解码线程           渲染线程
    │                   │                  │
    ├──► Play()        │                  │
    │                  ├──► ReadPacket()  │
    │                  ├──► Decode()      │
    │◄── FrameReady ───┤                  │
    │                  │                  ├──► Display()
    │                  │                  │
```

#### VideoDecoder 类

**职责**: 视频解码,使用现代 FFmpeg API

**关键设计**:
- 使用 `avcodec_send_packet` / `avcodec_receive_frame`
- RAII 管理 FFmpeg 资源
- 线程安全设计

```cpp
class VideoDecoder {
public:
    bool Initialize(AVCodecParameters* params);
    bool DecodePacket(AVPacket* packet, FrameData& output);
    
private:
    AVCodecContext* codec_ctx_;  // 自动管理生命周期
    AVFrame* frame_;
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

### 关键优化点

1. **零拷贝**: 尽量减少数据复制
2. **线程池**: 复用线程,减少创建开销
3. **内存池**: 预分配帧缓冲区
4. **异步 I/O**: 非阻塞读取

```cpp
// 内存池示例
class FramePool {
public:
    std::unique_ptr<FrameData> Acquire();
    void Release(std::unique_ptr<FrameData> frame);
    
private:
    std::queue<std::unique_ptr<FrameData>> pool_;
    std::mutex mutex_;
};
```

## 测试策略

### 单元测试

```cpp
TEST(VideoDecoderTest, InitializeSuccess) {
    VideoDecoder decoder;
    auto params = CreateTestCodecParams();
    
    EXPECT_TRUE(decoder.Initialize(params.get()));
    EXPECT_GT(decoder.GetWidth(), 0);
    EXPECT_GT(decoder.GetHeight(), 0);
}
```

### 集成测试

```cpp
TEST(MediaPlayerTest, PlayLocalFile) {
    MediaPlayer player;
    
    EXPECT_TRUE(player.Open("test.mp4"));
    player.Play();
    
    // 等待播放
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

### 短期 (1-3个月)

- [ ] 完成基础播放功能
- [ ] 实现流分析
- [ ] 添加人脸检测
- [ ] 完善 UI

### 中期 (3-6个月)

- [ ] 硬件解码支持
- [ ] 插件系统
- [ ] 更多分析算法
- [ ] 性能优化

### 长期 (6-12个月)

- [ ] AI 增强分析
- [ ] 实时协作
- [ ] 云端集成
- [ ] 移动端支持

---

**架构持续演进中...** 🚀
