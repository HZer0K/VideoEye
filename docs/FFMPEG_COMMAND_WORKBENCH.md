# FFmpeg 命令工作台

VideoEye 自带一整套基于 libav* 的分析能力，但有些事只有**原生 ffmpeg 程序**能做：
抽一段、转码验证、加滤镜、改封装、跑一遍响度归一化。这一页就是把那个程序接进 GUI ——
写命令、看实时输出、查参数含义。

它不是终端。这一条边界决定了后面所有的设计。

## 1. 能力边界

| 支持 | 不支持 |
| --- | --- |
| 单独一条 `ffmpeg` 命令及其参数 | `\|` 管道、`&&` / `;` 串联、`>` 重定向 |
| 含空格 / 中文的路径（加引号） | 把媒体数据输出到 `pipe:1` / `-` |
| 启动、实时看日志、停止、看退出码与耗时 | shell 变量、命令替换、交互式输入 |

检测到 shell 操作符时页面会直接拦下并说明原因，而不是"跑一半报错"。
输出目标解析到 stdout 时同样拦下 —— 日志框只装得下文本，二进制流灌进来只会把界面卡死。

## 2. 命令是怎么被执行的

```
命令文本 ──ParseCommandLine()──▶ QStringList 参数数组 ──QProcess::start(program, args)──▶ ffmpeg
```

**命令文本不交给系统 shell。** 分词规则（`core/ffmpeg/FfmpegCommandParser.cpp`）：

* `"` 与 `'` 成对生效，引号内的空格是路径的一部分；
* **反斜杠不是转义字符** —— Windows 路径处处是 `\`，照搬 POSIX 规则会把
  `D:\media\new\a.mp4` 吃掉反斜杠变成 `D:medianewa.mp4`；
* 引号没闭合 → 报"缺少一个引号"，不猜；
* 第一个 token 必须是 `ffmpeg` / `ffmpeg.exe` / 指向它的完整路径，也可以直接省略。

实际执行的可执行程序由 `FfmpegToolLocator` 解析，优先级：

1. 页面里手填的路径（存 `QSettings`，Linux/macOS 上系统 ffmpeg 不在 PATH 时用它）
2. 随包分发的那份（**默认没有** —— 只有打包者开了 `-DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON` 才有）
3. 构建期探测到的绝对路径（`cmake/deps/FfmpegTool.cmake` 写进生成头文件）
4. `PATH`
5. 各平台常见安装目录（详见第 5.1 节）

页面顶部会显示**实际使用的程序路径、来源和 `-version` 输出**。这一点很重要：
GUI 链接的是预编译开发包（用来做分析），执行的可能是系统里另一份 ffmpeg，两者版本可以不同。

## 3. 页面布局

```
┌──────────────────────────────────────────┬──────────────┐
│ ffmpeg 程序: [路径..............] [浏览…] │  指令字典     │
│ 程序/来源/版本/已加载的编码器数            │  [搜索......] │
│ ──────────────────────────────────────── │  [分类  ▾]   │
│ 命令: [ffmpeg -i "in.mp4" -crf 23 out.mp4]│  词条列表     │
│ [插入当前媒体路径] [插入输出路径…] [运行]  │              │
│ ──────────────────────────────────────── │  词条详情     │
│ [命令解释 | 运行日志 | 错误输出]           │  [插入到命令] │
│ 状态 / 退出码 / 耗时      [复制][保存]     │              │
└──────────────────────────────────────────┴──────────────┘
```

### 命令解释

按顺序把命令拆成"这一段在干什么"，并把流程串成一行：

```
输入文件 → 视频编码器 → 视频质量 (CRF) → 输出文件
```

点任意一项可以看到它的作用、适用位置、示例、常见取值与相关选项。

ffmpeg 的参数是**位置敏感**的：写在 `-i` 之前的输出侧选项不会报错，但也不会生效。
遇到这种情况解释器会点出来 —— 这是"参数写了没反应"最常见的原因。

**认不出来的参数会如实说认不出来。** 字典只收高频选项，ffmpeg 的私有选项
（`-x264-params` 之类）有几百个，编一个看似合理的解释比不解释更糟。

### 指令字典

按 全局 / 输入输出 / 时间与裁剪 / 流选择 / 编码 / 画面 / 音频 / 封装 分类，
支持关键词搜索（匹配参数名、中文标题、说明）。滤镜（`scale` / `loudnorm` 等）
也收在里面，「插入到命令」会自动补上 `-vf` / `-af`。

**编码器和滤镜是否可用，以当前这个 ffmpeg 的查询结果为准。** 页面启动（或改路径）时
会依次跑 `-version` / `-encoders` / `-filters` / `-formats`，把实际清单填进
`FfmpegCapabilityCache`。命令里写了当前 ffmpeg 没有的编码器时，解释区会直接标红提示，
不用等运行失败才知道。

## 4. 模块

```
core/ffmpeg/FfmpegCommandParser.h|.cpp      命令行文本 → 参数数组（含 shell 操作符检测）
core/ffmpeg/FfmpegProcessRunner.h|.cpp      QProcess 封装：异步 stdout/stderr、停止、退出码、耗时
core/ffmpeg/FfmpegToolLocator.h|.cpp        定位要执行的 ffmpeg 程序 + 解析版本号
core/ffmpeg/FfmpegCommandCatalog.h|.cpp     内置词条 + 当前 ffmpeg 的编码器/滤镜/格式清单
core/ffmpeg/FfmpegCommandExplainer.h|.cpp   参数数组 → 逐项解释 + 命令级提醒
ui/ffmpeg_panel/FfmpegPanel.h|.cpp          页面（编辑 / 输出 / 字典三区）
```

`FfmpegProcessRunner` 有几个不显眼但必要的处理：

* ffmpeg 的进度行（`frame= ... fps= ...`）**不换行**，靠 `\r` 反复覆盖同一行。
  按 `\n` 分行会攒出一条几十 KB 的长行，所以这里 `\r` 也当行分隔；
* 输出编码不保证是 UTF-8（Windows 中文环境下 ffmpeg 可能吐本地编码），
  解码失败会退到 `QString::fromLocal8Bit`；
* 「停止」先 `terminate()` 给进程 3 秒收尾时间（让它写出合法的 `moov` box），
  到点没退出再 `kill()`。

## 5. 用户机器上没装 ffmpeg 怎么办

**结论先说：这一页必须有原生 ffmpeg 程序，没法用链接进来的 libav* 替代。**
ffmpeg 命令行不是一层薄壳 —— 它是封装格式、滤镜图、编码器私有参数、时间轴与流选择
全部搅在一起的庞大前端（选项数以千计）。真要重实现一遍，既做不完，也不该向用户承诺
"和 ffmpeg 等价"。

所以正确的目标不是"没装也能跑命令"，而是让**缺程序成为一等状态**：
打包时尽量带上、运行时尽力找、真没有就给出可执行的补救步骤，并且不牵连其它功能。

### 5.1 查找顺序（`core/ffmpeg/FfmpegToolLocator.cpp`）

| 优先级 | 来源 | 说明 |
| --- | --- | --- |
| 1 | 用户在页面里指定的路径 | 用 QSettings 记住，重启后仍生效；路径不存在也会原样显示，方便排查 |
| 2 | 随包分发的 `ffmpeg.exe` | 仅当打包者显式开了 `-DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON` 才存在；默认没有（见「许可证」一节） |
| 3 | 构建期探测到的绝对路径 | configure 时写进 `FfmpegToolConfig.h` |
| 4 | `PATH` | 最常见的安装方式 |
| 5 | 各平台常见安装目录 | scoop / Chocolatey / winget / Homebrew / MacPorts / snap / flatpak / 手工解压目录 |

第 5 项专门解决"装了但没进 PATH"：Windows 上 scoop / choco 经常不写 PATH，
macOS 的 Homebrew 在 Apple Silicon 上跑到了 `/opt/homebrew/bin`。

### 5.2 页面上的表现

* 找不到时「运行」按钮**直接禁用**，原因写在 tooltip 里 —— 而不是让用户点了才蹦一个不知所云的错；
* 状态区下方浮出三个补救入口：**安装指引… / 打开下载页 / 重新检测**；
* 「安装指引…」按当前平台给出包管理器命令、手动安装步骤、官方下载页，并且可以一键复制成纯文本；
* 明说作用域：**只有这一页需要 ffmpeg**。媒体信息、流分析、QC 报告、播放器走的都是
  VideoEye 内置链接的 FFmpeg 库，不装 ffmpeg 一样能用；
* 指令字典与命令解释都是内置数据，离线可用，不受影响。

### 5.3 各平台怎么补

| 平台 | 推荐做法 |
| --- | --- |
| Windows | `winget install --id Gyan.FFmpeg -e` / `scoop install ffmpeg` / `choco install ffmpeg`；装完若没进 PATH，页面第 5 级查找（scoop/choco 常见目录）仍会找到 |
| macOS | `brew install ffmpeg`；装完在 `/opt/homebrew/bin`（Apple Silicon）或 `/usr/local/bin`（Intel），页面会自动扫这两个目录 |
| Linux | `sudo apt install -y ffmpeg` 等发行版命令 |

**三个平台都刻意不随包分发**，两条理由：一是许可证（见下面一节），二是把系统的 ffmpeg
拷进安装树会和系统的 `libav*` 版本打架。

打包者确实要随包时才用
`-DFFMPEG_TOOL=<path> -DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON`：Windows 上若那份 ffmpeg 不在
预编译包 `bin/` 里，需确认它自包含，否则只拷一个 exe 过去会以 `0xc0000135` 起不来。

## 6. 构建与打包

| 平台 | 做法 |
| --- | --- |
| Windows | `scripts/fetch-ffmpeg.ps1` **默认删除 `ffmpeg.exe`**（GPLv3，约 27MB）；本机要调试这一页就加 `-KeepFfmpegExe`。要不要进安装包另由 `-DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON` 决定，默认 OFF |
| Linux / macOS | 不打包系统 ffmpeg。构建期只记录路径，页面允许手动指定；改路径会重新查询该程序的编码器/滤镜清单 |

CMake 侧：`-DFFMPEG_TOOL=<path>` 可强制指定（路径不存在直接报错，不静默回退）。
构建机上没有 ffmpeg 是**正常状态**，只打一行 STATUS —— 构建用的是 `libav*` 开发包，
不是这个程序。

CI 冒烟：Windows 只在 `dist\bin\ffmpeg.exe` **存在时**才跑 `-version`（能跑就说明它依赖的
`av*.dll` 也在），不存在则跳过并 warning —— 默认不随包，缺它不是打包失败。Linux 同理。

## 7. 手工验收清单

1. `-version` —— 能看到版本，退出码 0
2. 含空格 / 中文路径的转码 —— 加引号后能正确执行
3. 无效参数 —— 退出码非 0，自动切到「错误输出」页
4. 缺失输入 —— ffmpeg 报错，页面显示"无法启动/失败"而不是静默
5. 已存在的输出文件 —— 不带 `-y` 时 ffmpeg 会卡住等输入，字典里 `-y` 说了原因
6. 运行中停止 —— 状态显示"已停止"，耗时正常，界面不卡
7. 长时间任务 —— 日志实时滚动，主线程不阻塞
8. 含 `\|` / `&&` 的命令 —— 被拦截并给出说明
9. `pipe:1` / `pipe:2` 输出 —— 被拦截，提示改成文件输出
   （`-f null -`、`-f md5 -` 这类只出文本的写法不拦；多输出命令里靠后的那个 `-f matroska -` 也要拦住）
10. 页面上手动指定到 `PATH` 里的 / 自定义路径的 ffmpeg —— 都能正确显示路径与版本；
    探测途中改路径，能力清单不会出现"旧版本 + 新编码器列表"的混杂结果
11. 机器上完全没有 ffmpeg —— 「运行」禁用且 tooltip 说明原因，补救条三个按钮可点，
    其余页面（媒体信息 / 流分析 / QC 报告 / 播放器）照常工作
12. 多输出命令（`ffmpeg -i in.mp4 -s hd480 out.mp4 -s hd720 out2.mp4`）—— 两个输出都被认成输出文件，
    写在它们之间的参数归属后一个输出；结构认不出来的项显示「未解析」而不是给纠正建议
13. 日志—— 「运行日志」按到达顺序合并 stdout/stderr（stderr 行以 `[err]` 开头），
    「错误输出」只留 stderr；正常转码的进度行出现在合并页里

## 8. 许可证：为什么默认不随包分发 ffmpeg

### 8.1 三个选项

| 选项 | 做法 | 代价 |
| --- | --- | --- |
| A. 改用 LGPL 构建 | 把链接与分发的 FFmpeg 换成 LGPL 构建（如 BtbN 的 `win64-lgpl-shared`），安装树里只留 LGPL 二进制 | 换源要重新锁版本与 SHA256、验证 ABI；LGPL 构建没有 `libx264`/`libx265`，导出视频会降级到 `libopenh264` / `mpeg4` |
| B. 不随包，提示用户自己装 | 分发物里没有 `ffmpeg.exe`，页面引导用户安装 | 该页开箱即用性下降，用户要多一步 |
| C. 接受 GPL | 项目从 MIT 改 GPLv3，随包分发完整 ffmpeg | 商业/闭源集成者直接劝退，MIT 带来的采纳优势全丢 |

### 8.2 我们的选择：B 打底，A 作为可切换目标，**不**选 C

VideoEye 的核心价值是**分析能力**，FFmpeg 命令工作台是辅助页面。为一个辅助页的开箱即用，
把整个项目的许可证自由度赔进去，性价比不成立 —— 这正是排除 C 的理由。

B 是默认状态，理由有三：

1. **MIT 是项目被采纳的原因之一。** 分析工具的使用者里有企业、广播与车机厂商，他们对许可证
   敏感；保持 MIT 才能被放心集成。
2. **用户自己装的 ffmpeg 反而更好。** 他们装到的通常是含 `libx264`/`libx265` 的完整版，
   功能强于我们能随包的任何一份；而用户自用不受分发条款约束 —— 这个义务不该由我们来担。
3. **不替用户做决定。** 打包者确信自己那份是 LGPL 构建、或愿意让分发物整体走 GPLv3 时，
   用 `-DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON` 显式打开并自行完成合规动作（随附许可证文本 +
   提供对应源码获取方式）。CMake 会在打开它时打 WARNING 提醒这件事。

### 8.3 需要如实说明的一件事

**当前锁定的 Windows 预编译包（gyan.dev `full_build-shared`）本身就是 GPLv3** —— 它包含
`libx264` 等 GPL-only 组件。VideoEye 链接它、并把 `av*.dll` 随包分发，因此严格来说
Windows 二进制已经处在 GPL 之下。这与"随不随包 `ffmpeg.exe`"是两个层次的问题：
不随包 CLI 消除了**额外分发一个 GPL 程序**，但没有消除库本身的授权状态。

真正要解决，走 A：把 `cmake/ffmpeg-version.json` 里的 Windows 包换成 LGPL 构建
（改 `windows.url` / `windows.sha256` 两个字段即可，脚本无需改动），并在分发物里随附
LGPL 文本与库的源码获取方式。我们已经把路铺好了 —— `MediaExporter` 不再硬依赖 `libx264`，
候选链是 `libx264 → libopenh264 → mpeg4`（webm 是 `libvpx-vp9 → libvpx`），
换成 LGPL 构建后导出功能照常可用，只是压缩率下降。

> 构建时的同一口径：`cmake/deps/FfmpegTool.cmake` 在"找到 ffmpeg 但不随包"时打的那句
> STATUS，说的也是上面这层关系 —— **不额外分发 GPL 程序 ≠ 分发物整体是 MIT**。
> 修改其中任何一处措辞时请同步另一处，别再让两处说法打架。

> 注意：Linux/macOS 上发行版与 Homebrew 的 ffmpeg 通常也启用了 `--enable-gpl`。
> 要彻底避开 GPL，那两个平台同样需要改用 LGPL 变体（多数要自行构建）。
