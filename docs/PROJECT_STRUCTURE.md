# VideoEye 2.0 项目结构

## 📁 清理后的目录结构

经过整理，根目录现在非常清爽！

### 根目录文件清单

```
VideoEye/
├── 📖 文档文件
│   ├── README.md                  # 项目说明文档
│   ├── QUICKSTART.md              # 快速入门指南
│   ├── BUILD_REPORT.md            # 编译运行报告
│   └── IMPLEMENTATION_SUMMARY.md  # 实施总结
│
├── 🔧 构建配置
│   ├── CMakeLists.txt             # CMake 主配置
│   ├── vcpkg.json                 # vcpkg 依赖配置
│   ├── .clang-format              # 代码格式化配置
│   ├── .gitignore                 # Git 忽略规则
│   ├── build.sh                   # Linux 构建脚本
│   └── build.bat                  # Windows 构建脚本
│
├── 📂 源代码目录
│   ├── core/                      # 核心业务层 ⭐
│   ├── ui/                        # UI 层 ⭐
│   ├── utils/                     # 工具类
│   └── tests/                     # 测试代码
│
├── 📦 资源目录
│   ├── resources/                 # 新架构资源
│   │   ├── icons/                # 图标
│   │   ├── styles/               # 样式
│   │   └── translations/         # 翻译
│   └── cmake/                     # CMake 模块
│
├── 📚 Legacy 目录
│   └── legacy/                    # 旧版代码归档 📦
│
├── 🛠️ 工具脚本
│   └── run.sh                     # 快速启动脚本
│
└── 🏗️ 构建输出
    └── build/                     # 编译输出 (git忽略)
```

---

## 🎯 核心源代码结构

### core/ - 核心业务层

```
core/
├── model/              # 数据模型
│   ├── FrameData.h/cpp    # 帧数据结构
│   └── StreamInfo         # 流信息结构
│
├── player/             # 播放器引擎
│   ├── MediaPlayer.h/cpp  # 媒体播放器 (Qt信号槽)
│   └── Decoders.h/cpp     # 音视频解码器
│
├── analyzer/           # 分析引擎 (待实现)
│   ├── StreamAnalyzer    # 流分析
│   ├── FrameAnalyzer     # 帧分析
│   └── FaceDetector      # 人脸检测
│
└── io/                 # 输入输出 (待实现)
    ├── InputStream       # 输入流管理
    └── DataExporter      # 数据导出
```

**已实现**: ✅ model, player  
**待实现**: ⏳ analyzer, io

---

### ui/ - 用户界面层

```
ui/
├── main_window/        # 主窗口
│   └── MainWindow.h/cpp  # Qt6 主窗口 ⭐
│
├── player_controls/    # 播放控制 (待实现)
│   ├── ControlPanel      # 控制面板
│   └── SeekBar           # 进度条
│
├── analysis_panel/     # 分析面板 (待实现)
│   ├── StreamInfoPanel   # 流信息面板
│   └── ChartPanel        # 图表面板
│
└── settings/           # 设置对话框 (待实现)
    └── SettingsDialog    # 设置窗口
```

**已实现**: ✅ main_window  
**待实现**: ⏳ player_controls, analysis_panel, settings

---

### utils/ - 工具类 (待实现)

```
utils/
├── Logger.h/cpp           # 日志系统
├── ConfigManager.h/cpp    # 配置管理
└── ThreadPool.h/cpp       # 线程池
```

---

### tests/ - 测试代码 (待实现)

```
tests/
├── unit/               # 单元测试
│   ├── test_decoder.cpp
│   └── test_player.cpp
│
└── integration/        # 集成测试
    └── test_playback.cpp
```

---

## 📦 Legacy 归档目录

```
legacy/
├── README.md              # 📖 Legacy 说明文档
│
├── mfc_legacy/            # MFC 旧版代码
│   ├── VideoEye.cpp/h        # 应用入口
│   ├── VideoEyeDlg.cpp/h     # 主对话框
│   ├── ve_play.cpp/h         # 播放逻辑
│   ├── Audiodecode.cpp/h     # 音频解码
│   ├── Videodecode.cpp/h     # 视频解码
│   ├── Rawanalysis.cpp/h     # 数据分析
│   ├── ...                   # 其他功能模块
│   ├── VideoEye.sln          # VS2010 解决方案
│   ├── VideoEye.vcxproj      # VS2010 项目
│   └── res/                  # 旧版资源文件
│
├── windows-dlls/          # Windows 动态库
│   ├── avcodec-54.dll        # FFmpeg (2012)
│   ├── opencv_*.dll          # OpenCV 2.3.1
│   ├── SDL.dll               # SDL 1.2
│   └── *.lib                 # 静态导入库
│
├── include/               # 旧版头文件
│   ├── libavcodec/
│   ├── libavformat/
│   ├── opencv/
│   └── ...
│
└── misc/                  # 其他旧文件
    ├── RTMPLive.xspf
    ├── ChangeLog
    └── *.suo, *.dmp
```

**说明**: Legacy 目录仅作为参考，不应直接编译使用。

---

## 📊 目录清理对比

### 清理前 (混乱)

```
VideoEye/
├── Assistantmediainfo.cpp
├── Audiodecode.cpp
├── cmdutils.cpp
├── Dataoutput.cpp
├── Dfanalysis.cpp
├── ... (30+ 个旧代码文件)
├── *.dll (18 个 DLL 文件)
├── VideoEye.sln
├── VideoEye.vcxproj
├── include/ (旧版头文件)
├── lib/ (旧版静态库)
└── ... (所有文件混在一起)
```

**问题**:
- ❌ 根目录 50+ 个文件
- ❌ 新旧代码混杂
- ❌ DLL 文件散落
- ❌ 难以导航

### 清理后 (整洁)

```
VideoEye/
├── core/ (新架构代码)
├── ui/ (新架构UI)
├── legacy/ (旧代码归档)
├── docs/ (文档)
└── 配置文件 (CMake, vcpkg, etc.)
```

**优势**:
- ✅ 根目录只有 15 个文件/目录
- ✅ 新旧代码分离
- ✅ 清晰的分层结构
- ✅ 易于导航

---

## 🎯 如何浏览代码

### 查看新架构代码

```bash
# 播放器引擎
cd core/player
ls -la

# UI 代码
cd ui/main_window
ls -la

# 数据模型
cd core/model
ls -la
```

### 参考旧版代码

```bash
# 查看 MFC 旧代码
cd legacy/mfc_legacy
ls -la

# 查看旧版主对话框
cat VideoEyeDlg.h | head -50

# 查看旧版播放逻辑
cat ve_play.cpp
```

---

## 📈 代码统计

### 新架构代码

| 目录 | 文件数 | 代码行数 | 状态 |
|------|--------|---------|------|
| core/ | 6 | ~600 | ✅ 已实现 |
| ui/ | 2 | ~300 | ✅ 已实现 |
| utils/ | 0 | 0 | ⏳ 待实现 |
| tests/ | 0 | 0 | ⏳ 待实现 |
| **总计** | **8** | **~900** | |

### Legacy 代码

| 目录 | 文件数 | 代码行数 | 用途 |
|------|--------|---------|------|
| mfc_legacy/ | 35+ | ~13500 | 参考 |
| windows-dlls/ | 30+ | - | 参考 |
| include/ | 100+ | - | 参考 |
| **总计** | **165+** | **~13500** | |

---

## 🗂️ 文件分类说明

### 🟢 应该关注的文件 (新架构)

- `CMakeLists.txt` - 构建配置
- `core/player/MediaPlayer.cpp` - 播放器核心
- `core/player/Decoders.cpp` - 解码器实现
- `ui/main_window/MainWindow.cpp` - UI 实现
- `README.md` - 项目文档

### 🟡 可以参考的文件 (Legacy)

- `legacy/mfc_legacy/VideoEyeDlg.cpp` - 旧版功能参考
- `legacy/mfc_legacy/ve_play.cpp` - 旧版播放逻辑
- `legacy/mfc_legacy/*.cpp` - 算法实现参考

### 🔴 不需要关心的文件

- `legacy/windows-dlls/*` - 旧版 Windows DLL
- `legacy/include/*` - 旧版头文件
- `legacy/misc/*` - 旧版辅助文件

---

## 🔄 目录整理原则

### 已应用的规则

1. **新旧分离**: 新架构代码在根目录，旧代码在 legacy/
2. **分层组织**: core/ (业务) + ui/ (界面) + utils/ (工具)
3. **功能聚合**: 相关功能放在同一目录
4. **资源集中**: 资源文件统一在 resources/
5. **文档齐全**: 每个目录都有清晰的文档

### 命名规范

- **目录**: 小写，下划线分隔 (如 `main_window`)
- **头文件**: PascalCase.h (如 `MediaPlayer.h`)
- **源文件**: PascalCase.cpp (如 `MediaPlayer.cpp`)
- **配置**: 小写，点分隔 (如 `.clang-format`)

---

## 🚀 下一步优化建议

### 短期

1. ✅ ~~整理根目录~~ (已完成)
2. ⏳ 完善视频帧显示
3. ⏳ 添加音频播放
4. ⏳ 实现基础分析功能

### 中期

1. ⏳ 添加单元测试
2. ⏳ 完善文档
3. ⏳ 性能优化
4. ⏳ 添加更多分析算法

### 长期

1. ⏳ 考虑删除 legacy/ (当所有功能都迁移完成后)
2. ⏳ 添加 CI/CD
3. ⏳ 发布正式版本

---

## 💡 使用提示

### 开发者

```bash
# 1. 查看新代码
cd core/player
code .

# 2. 编译运行
./build.sh

# 3. 参考旧代码
cd legacy/mfc_legacy
grep "avcodec_decode" *.cpp
```

### 使用者

```bash
# 直接运行
./run.sh

# 查看文档
cat README.md
cat QUICKSTART.md
```

---

**根目录现在非常整洁！** 🎉

从 50+ 个文件减少到 15 个，清晰的分层结构，易于导航和维护！
