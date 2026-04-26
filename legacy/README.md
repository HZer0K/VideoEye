# VideoEye 旧版代码 (MFC 版本)

## 📁 目录说明

本目录包含 VideoEye 1.0 的旧版代码，基于 MFC 和 Visual Studio 2010 开发。

这些代码已不再使用，但保留作为参考，方便迁移时查阅功能实现。

---

## 📂 子目录结构

### mfc_legacy/

包含所有 MFC 版本的源代码和项目文件。

#### 核心文件

| 文件 | 说明 |
|------|------|
| `VideoEye.cpp/h` | 应用程序入口点 |
| `VideoEyeDlg.cpp/h` | 主对话框类 (240行头文件，所有功能耦合) |
| `ve_play.cpp/h` | 播放控制逻辑 |
| `stdafx.cpp/h` | 预编译头文件 |

#### 功能模块

| 文件 | 说明 |
|------|------|
| `Audiodecode.cpp/h` | 音频解码对话框 |
| `Videodecode.cpp/h` | 视频解码对话框 |
| `InputList.cpp/h` | 输入列表管理 |
| `Sysinfo.cpp/h` | 系统信息显示 |
| `Dataoutput.cpp/h` | 数据输出功能 |
| `Rawanalysis.cpp/h` | 原始数据分析 |
| `Dfanalysis.cpp/h` | 微分分析 |
| `Dfanalysispic.cpp/h` | 微分分析图片 |
| `Optplayer.cpp/h` | 优化播放器 |
| `Assistantmediainfo.cpp/h` | 媒体信息助手 |
| `Welcome.cpp/h` | 欢迎界面 |
| `cmdutils.cpp/h` | 命令行工具函数 |

#### 项目文件

| 文件 | 说明 |
|------|------|
| `VideoEye.sln` | Visual Studio 2010 解决方案 |
| `VideoEye.vcxproj` | Visual C++ 项目文件 |
| `VideoEye.rc` | 资源文件 |
| `VideoEye.aps` | 资源二进制文件 |
| `resource.h` | 资源头文件 |

#### 配置文件

| 文件 | 说明 |
|------|------|
| `config.h` | FFmpeg 配置头文件 |
| `targetver.h` | 目标 Windows 版本 |
| `haarcascade_frontalface_alt2.xml` | OpenCV 人脸检测级联分类器 |

#### 其他文件

| 文件 | 说明 |
|------|------|
| `MediaInfoDLL.h` | MediaInfo DLL 接口 |
| `ReadMe.txt` | 旧版项目说明 |
| `问题.txt` | 已知问题记录 |
| `InputList.m3u` | 播放列表 (M3U格式) |
| `InputList.xspf` | 播放列表 (XSPF格式) |

---

### windows-dlls/

包含 Windows 平台的旧版动态链接库和静态库。

#### FFmpeg DLL (2012版本)

- `avcodec-54.dll` - 编解码库
- `avformat-54.dll` - 格式处理库
- `avutil-51.dll` - 工具库
- `avdevice-54.dll` - 设备库
- `avfilter-3.dll` - 滤镜库
- `swscale-2.dll` - 图像缩放库
- `swresample-0.dll` - 音频重采样库
- `postproc-52.dll` - 后处理库

#### OpenCV DLL (2.3.1版本)

- `opencv_core231.dll` - 核心库
- `opencv_highgui231.dll` - 高级 GUI 库
- `opencv_imgproc231.dll` - 图像处理库
- `opencv_objdetect231.dll` - 目标检测库
- `opencv_calib3d231.dll` - 3D 校准库
- `opencv_features2d231.dll` - 2D 特征库
- `opencv_flann231.dll` - 快速匹配库
- `tbb.dll` - Intel TBB 并行库

#### 其他 DLL

- `SDL.dll` - SDL 多媒体库
- `MediaInfo.dll` - 媒体信息库

#### 静态库 (.lib)

对应的导入库文件，用于 Visual Studio 链接。

---

## 🔍 与新版代码对比

### 旧版架构 (MFC)

```
VideoEye/
├── VideoEye.cpp          # 应用入口
├── VideoEyeDlg.cpp       # 主对话框 (所有功能)
├── ve_play.cpp          # 播放逻辑
├── Audiodecode.cpp      # 音频解码
├── Videodecode.cpp      # 视频解码
├── Rawanalysis.cpp      # 数据分析
└── ...                  # 其他功能模块
```

**问题**:
- ❌ 所有功能耦合在一个对话框类
- ❌ 240行头文件，难以维护
- ❌ 使用全局变量
- ❌ 内存管理不规范
- ❌ 仅支持 Windows
- ❌ 字符编码混乱 (导致乱码)

### 新版架构 (Qt6)

```
VideoEye/
├── core/
│   ├── player/          # 播放器引擎
│   │   ├── MediaPlayer.cpp/h
│   │   └── Decoders.cpp/h
│   └── model/           # 数据模型
│       └── FrameData.cpp/h
├── ui/
│   └── main_window/     # 主窗口
│       └── MainWindow.cpp/h
├── utils/               # 工具类
└── tests/               # 测试
```

**优势**:
- ✅ 清晰的分层架构
- ✅ 模块化设计
- ✅ 智能指针管理
- ✅ 跨平台支持
- ✅ UTF-8 编码
- ✅ 现代 C++17

---

## 📖 如何参考旧代码

### 查看功能实现

如果你想了解某个功能在旧版中的实现：

```bash
# 查看旧版主对话框
cat legacy/mfc_legacy/VideoEyeDlg.h

# 查看播放逻辑
cat legacy/mfc_legacy/ve_play.cpp

# 查看解码实现
cat legacy/mfc_legacy/Videodecode.cpp
```

### 对比新旧实现

```bash
# 新旧播放器对比
diff legacy/mfc_legacy/ve_play.cpp core/player/MediaPlayer.cpp

# 查看旧版使用了哪些 FFmpeg API
grep "avcodec_decode" legacy/mfc_legacy/*.cpp
```

### 迁移功能

如果需要将旧功能迁移到新架构：

1. **理解旧代码逻辑**
2. **使用现代 API 重写**
3. **保持接口清晰**
4. **添加单元测试**

---

## ⚠️ 注意事项

### 不要直接使用

这些代码**不应该**被直接编译或运行，原因：

1. 需要 Windows 环境
2. 需要 Visual Studio 2010
3. 使用过时的库版本
4. 存在内存泄漏风险
5. 编码问题导致乱码

### 仅作参考

仅用于：
- ✅ 了解业务逻辑
- ✅ 参考算法实现
- ✅ 对比新旧架构
- ✅ 学习迁移经验

---

## 🗑️ 何时可以删除

当以下条件都满足时，可以删除此目录：

- [ ] 所有功能都已在新架构中实现
- [ ] 所有算法都已迁移并测试
- [ ] 团队确认不再需要参考
- [ ] 项目稳定运行 3 个月以上

---

## 📊 代码统计

### 旧版代码量

| 类型 | 文件数 | 代码行数 |
|------|--------|---------|
| 源代码 (.cpp) | 16 | ~8000 |
| 头文件 (.h) | 16 | ~3000 |
| 资源文件 | 5 | ~2000 |
| 配置文件 | 5 | ~500 |
| **总计** | **42** | **~13500** |

### 新版代码量 (当前)

| 类型 | 文件数 | 代码行数 |
|------|--------|---------|
| 核心层 | 6 | ~600 |
| UI层 | 2 | ~300 |
| 配置 | 8 | ~200 |
| **总计** | **16** | **~1100** |

---

## 🎯 迁移进度

### 已迁移功能

- ✅ 主窗口框架
- ✅ 播放器引擎
- ✅ 视频解码 (现代 API)
- ✅ 音频解码 (现代 API)
- ✅ 播放控制
- ✅ 进度管理

### 待迁移功能

- [ ] 视频帧显示 (YUV → RGB)
- [ ] 音频播放 (SDL2)
- [ ] 流分析功能
- [ ] 直方图分析
- [ ] 边缘检测
- [ ] 人脸检测
- [ ] 数据输出
- [ ] 媒体信息显示
- [ ] 播放列表管理

---

**保留旧代码，拥抱新架构！** 🚀
