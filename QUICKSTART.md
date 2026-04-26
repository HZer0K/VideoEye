# VideoEye 2.0 快速入门指南

## 🚀 5分钟快速开始

### 前置条件

确保你的系统已安装:
- CMake 3.20+
- C++17 编译器 (GCC 9+, Clang 9+, MSVC 2019+)
- 依赖库 (FFmpeg, Qt6, OpenCV, SDL2)

### 步骤 1: 检查依赖

```bash
# Ubuntu/Debian
cmake --version
g++ --version
pkg-config --modversion libavformat
pkg-config --modversion Qt6Core
pkg-config --modversion opencv4
```

### 步骤 2: 安装依赖 (如果未安装)

#### Ubuntu 22.04+

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake \
    qt6-base-dev qt6-multimedia-dev qt6-charts-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    libopencv-dev libsdl2-dev
```

#### macOS

```bash
brew install cmake qt ffmpeg opencv sdl2
```

### 步骤 3: 构建项目

```bash
# 使用自动构建脚本
./build.sh

# 或手动构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 步骤 4: 运行

```bash
# 从项目根目录
./build/bin/VideoEye

# 或
./build.sh  # 构建完成后会提示运行命令
```

---

## 📖 详细指南

### 使用 vcpkg 管理依赖 (推荐)

如果你使用的是 Windows 或者想要更简单的依赖管理:

```bash
# 1. 安装 vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh  # Linux/macOS
# .\bootstrap-vcpkg.bat  # Windows

# 2. 安装 VideoEye 依赖
./vcpkg install \
    ffmpeg \
    opencv4 \
    sdl2 \
    qt6-base \
    qt6-multimedia \
    qt6-charts

# 3. 集成到系统 (Windows)
# .\vcpkg integrate install

# 4. 构建 VideoEye
cd /path/to/VideoEye
mkdir build && cd build
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
make -j$(nproc)
```

### 常见问题

#### Q1: CMake 找不到 FFmpeg

**解决方案**:

```bash
# 方法1: 设置 PKG_CONFIG_PATH
export PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/share/pkgconfig

# 方法2: 手动指定 FFmpeg 路径
cmake .. \
    -DFFMPEG_INCLUDE_DIR=/usr/include \
    -DAVCODEC_LIBRARY=/usr/lib/libavcodec.so \
    -DAVFORMAT_LIBRARY=/usr/lib/libavformat.so
```

#### Q2: Qt6 版本过低

**解决方案**:

```bash
# Ubuntu: 添加 Qt PPA
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install qt6-base-dev

# 或者从源码编译 Qt6
```

#### Q3: 编译时内存不足

**解决方案**:

```bash
# 减少并行编译数量
make -j2  # 只使用2个线程

# 或者启用交换空间
sudo fallocate -l 4G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
```

---

## 🎯 测试功能

### 播放本地文件

1. 启动 VideoEye
2. 点击 "文件" -> "打开文件"
3. 选择一个视频文件 (支持 mp4, avi, mkv, flv 等)
4. 点击播放按钮

### 播放网络流

1. 点击 "文件" -> "打开URL"
2. 输入流媒体地址,例如:
   - RTMP: `rtmp://live.example.com/stream`
   - RTSP: `rtsp://camera.example.com/stream`
   - HTTP: `http://example.com/video.mp4`
3. 点击确定

### 查看流信息

打开文件后,在底部的"流信息"标签页可以看到:
- 文件名和格式
- 视频编码、分辨率、帧率
- 音频编码、采样率、声道数
- 时长和码率

---

## 🔧 开发提示

### IDE 配置

#### VS Code

1. 安装扩展:
   - C/C++ (Microsoft)
   - CMake Tools (Microsoft)
   - Qt for Python (可选)

2. 打开项目文件夹
3. CMake Tools 会自动检测配置
4. 按 `F7` 构建, `Ctrl+F5` 运行

#### CLion

1. 直接打开项目文件夹
2. CLion 会自动识别 CMakeLists.txt
3. 在右上角配置运行/调试配置

#### Visual Studio 2022

```bash
# 生成 VS 项目
cmake .. -G "Visual Studio 17 2022" -A x64

# 打开解决方案
devenv VideoEye.sln
```

### 代码格式化

```bash
# 格式化单个文件
clang-format -i core/player/MediaPlayer.cpp

# 格式化所有文件
find core ui utils -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

### 调试技巧

```bash
# 1. Debug 构建
./build.sh debug

# 2. 使用 GDB
gdb build-debug/bin/VideoEye

# 3. 设置断点
(gdb) break MediaPlayer::Open
(gdb) run

# 4. 查看调用栈
(gdb) backtrace
```

---

## 📚 下一步

- 📖 阅读 [完整文档](README.md)
- 🔍 了解 [架构设计](docs/ARCHITECTURE.md)
- 🤝 查看 [贡献指南](CONTRIBUTING.md)
- 📝 阅读 [API 文档](docs/API.md)

---

## 💬 获取帮助

遇到问题? 

1. 查看 [常见问题](#常见问题)
2. 搜索 [Issues](https://github.com/yourusername/VideoEye/issues)
3. 提交新的 Issue
4. 加入社区讨论

---

**祝你使用愉快!** 🎉
