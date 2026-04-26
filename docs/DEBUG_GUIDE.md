# VideoEye 2.0 闪退问题Debug完整指南

## 🎯 问题

打开视频文件后程序闪退。

---

## ✅ 已实施的修复

### 修复内容

我已经在 `core/player/MediaPlayer.cpp` 的 `DecodeThread()` 函数中添加了以下保护措施：

#### 1. **帧数据验证**
```cpp
// 验证帧数据有效性 (防止崩溃)
if (frame_data.width <= 0 || frame_data.height <= 0 || !frame_data.data[0]) {
    LOG_WARN("跳过无效帧数据");
    av_packet_unref(packet);
    continue;
}
```

#### 2. **QImage有效性检查**
```cpp
QImage qimage(frame_data.width, frame_data.height, QImage::Format_RGB32);

if (!qimage.isNull()) {
    emit FrameReady(qimage);
}
```

#### 3. **分析器异常处理**
```cpp
// 直方图分析
try {
    auto hist = frame_analyzer_.ComputeHistogram(frame_data);
    emit HistogramReady(hist);
} catch (const std::exception& e) {
    LOG_ERROR("直方图分析失败: " + std::string(e.what()));
}

// 人脸检测
try {
    auto faces = face_detector_.DetectFaces(frame_data);
    if (!faces.empty()) {
        emit FaceDetectionReady(faces);
    }
} catch (const std::exception& e) {
    LOG_ERROR("人脸检测失败: " + std::string(e.what()));
}
```

---

## 🧪 测试步骤

### 1. 重新编译（已完成）

```bash
cd /home/hxk/project/VideoEye/VideoEye/build
make -j$(nproc)
```

✅ 编译成功！

### 2. 运行程序测试

```bash
cd /home/hxk/project/VideoEye/VideoEye
./run.sh
```

然后：
1. 点击"文件" → "打开文件"
2. 选择一个视频文件（推荐先用小文件测试）
3. 观察是否还会闪退

### 3. 查看日志输出

程序会在终端输出日志，注意观察：
- 是否有 `[WARN] 跳过无效帧数据`
- 是否有 `[ERROR] 分析失败`
- 崩溃前的最后一条日志是什么

---

## 🔍 如果仍然崩溃，使用以下方法深入Debug

### 方法1: 使用GDB（推荐）

```bash
cd /home/hxk/project/VideoEye/VideoEye/build

# 启动GDB
gdb ./bin/VideoEye

# GDB内执行
(gdb) run
# 在程序中打开视频文件
# 等待崩溃

# 崩溃后执行
(gdb) bt              # 查看完整的backtrace
(gdb) bt full         # 查看局部变量
(gdb) frame 0         # 查看崩溃的帧
(gdb) list            # 查看周围代码
(gdb) print frame_data # 打印变量值
(gdb) info registers  # 查看寄存器状态
```

**保存调试信息**：
```bash
(gdb) set logging on
(gdb) run
# ... 崩溃 ...
(gdb) bt full
(gdb) set logging off

# 查看日志
cat gdb.txt
```

---

### 方法2: 使用Valgrind检测内存错误

```bash
cd /home/hxk/project/VideoEye/VideoEye/build

valgrind --tool=memcheck \
         --leak-check=full \
         --track-origins=yes \
         --show-leak-kinds=all \
         --log-file=valgrind.log \
         ./bin/VideoEye
```

**查看关键错误**：
```bash
# 搜索常见错误
grep -i "invalid" valgrind.log | head -20
grep -i "uninitialized" valgrind.log | head -20
grep -i "segfault" valgrind.log
```

---

### 方法3: 使用AddressSanitizer（最强工具）

重新编译启用ASAN：

```bash
cd /home/hxk/project/VideoEye/VideoEye/build

# 清理旧编译
make clean

# 使用ASAN重新配置
cmake .. -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
         -DCMAKE_BUILD_TYPE=Debug

# 编译
make -j$(nproc)

# 运行（会自动报告错误）
./bin/VideoEye
```

ASAN会输出详细的错误报告，包括：
- 栈使用错误
- 堆缓冲区溢出
- 全局缓冲区溢出
- Use-after-free
- 内存泄漏

---

### 方法4: 添加更多调试日志

如果上述工具都不够，可以添加更详细的日志：

#### 在 `MediaPlayer::Open()` 中添加：

```cpp
bool MediaPlayer::Open(const QString& url) {
    LOG_DEBUG("===== Open() 开始 =====");
    LOG_DEBUG("URL: " + url.toStdString());
    
    // ... 原有代码 ...
    
    LOG_DEBUG("format_ctx_: " + std::string(format_ctx_ ? "valid" : "null"));
    LOG_DEBUG("video_stream_index: " + std::to_string(video_stream_index_));
    LOG_DEBUG("audio_stream_index: " + std::to_string(audio_stream_index_));
    
    if (video_decoder_) {
        LOG_DEBUG("video_decoder: valid");
        LOG_DEBUG("video size: " + std::to_string(video_decoder_->GetWidth()) + "x" +
                  std::to_string(video_decoder_->GetHeight()));
    }
    
    LOG_DEBUG("===== Open() 完成 =====");
    return true;
}
```

#### 在 `DecodeThread()` 开头添加：

```cpp
void MediaPlayer::DecodeThread() {
    LOG_DEBUG("===== DecodeThread 启动 =====");
    LOG_DEBUG("should_stop_: " + std::to_string(should_stop_.load()));
    LOG_DEBUG("video_stream_index: " + std::to_string(video_stream_index_));
    LOG_DEBUG("format_ctx: " + std::string(format_ctx_ ? "valid" : "null"));
    
    AVPacket* packet = av_packet_alloc();
    model::FrameData frame_data;
    
    int frame_count = 0;
    
    while (!should_stop_) {
        // ... 读取包 ...
        
        LOG_DEBUG("读取包: stream=" + std::to_string(packet->stream_index) +
                  ", size=" + std::to_string(packet->size));
        
        if (packet->stream_index == video_stream_index_) {
            frame_count++;
            LOG_DEBUG("解码第 " + std::to_string(frame_count) + " 帧");
            
            if (video_decoder_->DecodePacket(packet, frame_data)) {
                LOG_DEBUG("解码成功: " + std::to_string(frame_data.width) + "x" +
                          std::to_string(frame_data.height));
                LOG_DEBUG("data[0]: " + std::string(frame_data.data[0] ? "valid" : "null"));
            } else {
                LOG_DEBUG("解码失败");
            }
        }
        
        // ...
    }
    
    LOG_DEBUG("===== DecodeThread 退出 =====");
}
```

---

## 📊 常见崩溃原因对照表

| 症状 | 可能原因 | 解决方法 |
|------|---------|---------|
| 打开文件立即崩溃 | format_ctx未正确初始化 | 检查Open()返回值 |
| 第一帧解码时崩溃 | frame_data未初始化 | ✅ 已修复 |
| 播放几秒后崩溃 | 内存泄漏/缓冲区溢出 | 使用Valgrind |
| 随机崩溃 | 线程竞争 | 添加mutex保护 |
| 启用分析后崩溃 | 分析器访问空指针 | ✅ 已添加验证 |

---

## 🎯 测试建议

### 渐进式测试

1. **最简测试**
   ```bash
   # 禁用所有分析功能
   # 只测试基本播放
   ```

2. **逐步启用**
   ```
   1. 先测试基本播放（不启用分析）
   2. 启用流分析
   3. 启用直方图
   4. 启用人脸检测
   ```

3. **使用不同文件**
   ```
   - 小文件 (< 10MB)
   - 不同格式 (mp4, avi, mkv)
   - 不同编码 (H.264, H.265, MPEG4)
   - 不同分辨率 (360p, 720p, 1080p)
   ```

---

## 📝 报告问题的模板

如果修复后仍然崩溃，请提供以下信息：

```
## 崩溃报告

### 基本信息
- OS: Linux Debian 12
- 视频文件: xxx.mp4 (大小, 分辨率, 编码)
- 崩溃时机: 打开文件时 / 播放X秒后

### 日志输出
```
[粘贴终端输出的最后50行日志]
```

### GDB Backtrace
```
[粘贴 bt full 的输出]
```

### Valgrind错误（如有）
```
[粘贴valgrind检测到的错误]
```

### 重现步骤
1. 启动程序
2. 点击 文件 -> 打开文件
3. 选择 xxx.mp4
4. 程序崩溃
```

---

## 🚀 快速测试命令

```bash
# 1. 编译（已完成）
cd /home/hxk/project/VideoEye/VideoEye/build
make -j$(nproc)

# 2. 运行
cd ..
./run.sh

# 3. 如果崩溃，使用GDB
cd build
gdb ./bin/VideoEye
(gdb) run
# 打开视频
# 崩溃后
(gdb) bt full
(gdb) quit

# 4. 查看日志
cat ../videoeye.log 2>/dev/null || echo "无日志文件"
```

---

## ✨ 预期结果

应用修复后：
- ✅ 无效帧会被跳过而不是崩溃
- ✅ QImage创建前会验证尺寸
- ✅ 分析器异常会被捕获并记录
- ✅ 终端会显示详细的错误日志

现在请测试一下，看看是否还会崩溃！如果还有问题，使用上述debug工具可以提供更多信息。
