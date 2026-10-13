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

实际执行的可执行程序按下面的规则定：

* **命令里写了完整路径**（`C:\tools\ffmpeg.exe -i in.mp4 out.mp4`）→ 就执行那一个程序。
  这是原生命令行的直觉：把某个路径写在命令头上，意思就是要跑它。路径不存在会直接拦下，
  不会悄悄换成别的一个。
* 只写了裸的 `ffmpeg`（或省略程序名）→ 用 `FfmpegToolLocator` 解析出来的那个，优先级：

  1. 页面里手填的路径（存 `QSettings`，Linux/macOS 上系统 ffmpeg 不在 PATH 时用它）
  2. 随包分发的那份（**默认没有** —— 只有打包者开了 `-DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON` 才有）
  3. 构建期探测到的绝对路径（**仅 Debug 构建**；见下）
  4. `PATH`
  5. 各平台常见安装目录（详见第 5.1 节）
  6. 构建期探测到的绝对路径（兜底）

  第 3 项为什么要分两处：构建期路径是**构建机**的事实。开发机自己跑它最合适，
  但安装版如果还认它，用户新装/升级了 PATH 里的 ffmpeg 也仍会用构建机那一份，
  安装包还要凭空依赖一台它不该知道的开发机。所以 Release 下它降级成兜底
  （开关：`-DVIDEOEYE_FFMPEG_TOOL_PREFER_BUILDTIME=ON|OFF`，默认随 Debug 打开）。

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
* 「停止」分三档递进，**先保证输出文件是完整的**：

  1. 向 stdin 写一个 `q\n` —— ffmpeg 官方支持的交互方式，它会正常收尾并写出 `moov` box；
  2. 3 秒后还没退出 → `terminate()`（Unix 的 SIGTERM 会被 ffmpeg 接住并按正常流程收尾）；
  3. 再 3 秒还没退出 → `kill()`。

  为什么必须先试 `q`：Windows 控制台版 ffmpeg 既不响应 `terminate()` 也没有 CTRL_BREAK
  可发，直接强杀只会得到一个没有 `moov` box 的 mp4 —— 实测 48 字节、`ffprobe` 报
  `moov atom not found`；而写 `q` 那次退出码 0、输出文件完整可读（21.24s）。
  唯一例外是命令里带 `-nostdin`：那时 ffmpeg 根本不读 stdin，写进去没人接，
  页面会提前说明"无法优雅收尾，输出可能被截断"。
  结果里的 `stop_stage` 会告诉界面走到了哪一档，被强杀时状态栏显示
  **「已停止（输出可能不可用）」** 而不是一律显示"已停止"。

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
| 3 | 构建期探测到的绝对路径 | 仅 Debug 构建插队在这里；Release 下让位给 PATH（见第 2 节） |
| 4 | `PATH` | 最常见的安装方式 |
| 5 | 各平台常见安装目录 | scoop / Chocolatey / winget / Homebrew / MacPorts / snap / flatpak / 手工解压目录 |
| 6 | 构建期探测到的绝对路径 | 兜底：本机哪儿都找不到时的最后一次尝试 |

第 5 项专门解决"装了但没进 PATH"：Windows 上 scoop / choco 经常不写 PATH，
macOS 的 Homebrew 在 Apple Silicon 上跑到了 `/opt/homebrew/bin`。

另外还有一种不在表里的情况：**命令里直接写了完整路径**（`C:\tools\ffmpeg.exe -i ...`）。
那一条命令就执行那一个程序，不走上面任何一项（详见第 2 节）。

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

打包者确实要随包时才用 `-DFFMPEG_TOOL=<path> -DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON`。
这个开关**只支持 Windows**：Linux 的 `.so` 与 macOS 的 `@rpath`/签名都不是"复制一个文件"
能解决的事，配置了直接 `FATAL_ERROR`（而不是给一个必然坏掉的安装树）。

Windows 上也不再有"自包含"这个前提：configure 期会对那个 `ffmpeg.exe` 跑一次
`file(GET_RUNTIME_DEPENDENCIES)`，把它真正依赖的 DLL（只限 ffmpeg 自己目录 /
预编译包 `bin/` 里的那些，系统 DLL 不跟着分发）一起纳入构建期拷贝与 `install()`；
解析不出来的依赖会明确列出来警告。

## 6. 构建与打包

| 平台 | 做法 |
| --- | --- |
| Windows | `scripts/fetch-ffmpeg.ps1` **默认删除 `ffmpeg.exe`**（约 30MB，且只服务于本页）；本机要调试这一页就加 `-KeepFfmpegExe`。要不要进安装包另由 `-DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON` 决定，默认 OFF |
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
6. 运行中停止 —— 状态显示"已停止（正常收尾）"，耗时正常，界面不卡；
   **停完拿 ffprobe 打开输出文件，必须能读出时长**（这才是"停止没弄坏文件"的判据）。
   命令里带 `-nostdin` 时改成显示"已停止（输出可能不可用）"，并附有警告
7. 长时间任务 —— 日志实时滚动，主线程不阻塞
8. 含 `\|` / `&&` 的命令 —— 被拦截并给出说明
9. `pipe:1` / `pipe:2` 输出 —— 被拦截，提示改成文件输出
   （`-f null -`、`-f md5 -` 这类只出文本的写法不拦；多输出命令里靠后的那个 `-f matroska -` 也要拦住）
10. 命令里写完整路径（`C:\tools\ffmpeg.exe -i a.mp4 b.mp4`）—— 日志抬头显示的就是那一个程序，
    页面设置里的另一个路径会被明确标注"已被忽略"；路径不存在时直接拦下并提示
11. 信息查询命令（`-version` / `-encoders` / `-formats`）—— 只解释它的查询作用，
    不出现"没有输入文件 / 没有输出文件"
10. 页面上手动指定到 `PATH` 里的 / 自定义路径的 ffmpeg —— 都能正确显示路径与版本；
    探测途中改路径，能力清单不会出现"旧版本 + 新编码器列表"的混杂结果
11. 机器上完全没有 ffmpeg —— 「运行」禁用且 tooltip 说明原因，补救条三个按钮可点，
    其余页面（媒体信息 / 流分析 / QC 报告 / 播放器）照常工作
12. 多输出命令（`ffmpeg -i in.mp4 -s hd480 out.mp4 -s hd720 out2.mp4`）—— 两个输出都被认成输出文件，
    写在它们之间的参数归属后一个输出；结构认不出来的项显示「未解析」而不是给纠正建议
13. 日志—— 「运行日志」按到达顺序合并 stdout/stderr（stderr 行以 `[err]` 开头），
    「错误输出」只留 stderr；正常转码的进度行出现在合并页里

## 8. 许可证：Windows 锁定包已切到 LGPLv3

### 8.1 三个选项（已选 A）

| 选项 | 做法 | 代价 | 状态 |
| --- | --- | --- | --- |
| A. 改用 LGPL 构建 | 把链接与分发的 FFmpeg 换成 LGPL 构建（BtbN `win64-lgpl-shared`） | 换源要重新锁版本与 SHA256、验证 ABI；LGPL 构建没有 `libx264`/`libx265`，导出视频降级到 `libopenh264` / `mpeg4` | ✅ **已采纳** |
| B. 不随包 `ffmpeg.exe`，提示用户自己装 | 分发物里没有 CLI，页面引导用户安装 | 该页开箱即用性下降，用户要多一步 | ✅ 同时保留（体积与用途取舍） |
| C. 接受 GPL | 项目从 MIT 改 GPLv3，随包分发完整 ffmpeg | 商业/闭源集成者直接劝退 | ❌ 排除 |

### 8.2 现在的实际状态

| 平台 | FFmpeg | 许可证 |
| --- | --- | --- |
| Windows | BtbN `win64-lgpl-shared`，`n8.1.3-9-g29e619e767`（钉在 `autobuild-2026-09-30-13-08`） | **LGPL-3.0-or-later** |
| Linux / macOS | 系统 FFmpeg（`pkg-config`） | 取决于发行版构建，**多数带 `--enable-gpl`** |

LGPL 构建的配置里明确 `--disable-libx264 --disable-libx265 --enable-libopenh264`，
正好接上 `MediaExporter` 的候选链 `libx264 → libopenh264 → mpeg4`（webm 是
`libvpx-vp9 → libvpx`）：导出功能照常可用，只是压缩率下降 —— 这是刻意接受的取舍。

`ffmpeg.exe` 仍然默认删除，但理由**不再是许可证**（LGPL 构建随包没问题），而是
约 30MB 的体积与"只服务于这一个辅助页面"。要随包就 `-KeepFfmpegExe` +
`-DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON`。

### 8.3 分发时必须履行的三条（LGPLv3 的要求）

1. **随附许可证文本。** LGPLv3 是 GPLv3 的"附加许可"，只给 LGPLv3 等于只给了一半，
   两份全文都要有。现在由 `cmake/deps/ThirdPartyNotices.cmake` 在每次 `install` 时
   生成，安装树里会有：
   `doc/THIRD_PARTY_NOTICES.md`、`doc/licenses/FFmpeg-LGPL-3.0-or-later.txt`、
   `doc/licenses/GPL-3.0.txt`、`doc/licenses/LGPL-3.0.txt`、
   `doc/licenses/Qt6-copyright.txt`、`doc/licenses/VideoEye-MIT.txt`。
   **没有这些文件的二进制包不要发布。**
2. **提供对应源码的获取方式。** `THIRD_PARTY_NOTICES.md` 里写了 `ffmpeg.org/download.html`
   与锁定包的 release tag（内含 FFmpeg 源码 commit `g29e619e767`）。BtbN 的每个
   release 都对应一次可复现的构建，这就是 LGPL 要求的"对应源码"。
3. **不限制使用者替换这份库。** 已满足：VideoEye 以动态库方式使用 FFmpeg，
   使用者可以自行编译同版本库直接替换文件，无需重新编译 VideoEye。声明里也写明了这一点。

### 8.4 仍然存在的边界 —— 不能说"分发物整体是 MIT"

- **Linux / macOS 用系统 FFmpeg**，多数发行版与 Homebrew 的构建带 GPL 组件。
  这两个平台的分发物**仍然受 GPL 约束**，除非改用 LGPL 变体或自行静态构建。
  `THIRD_PARTY_NOTICES.md` 在用系统 FFmpeg 时会如实标注"许可证取决于发行版构建"。
- **Qt6 是 LGPLv3**（另有 GPL / 商业授权可选）。
- 所以对外表述只能是：**VideoEye 源码 MIT；Windows 分发物含 LGPLv3 组件；
  Linux/macOS 取决于系统 FFmpeg 的构建配置。**

> 构建时的同一口径：`cmake/deps/FfmpegTool.cmake` 在"找到 ffmpeg 但不随包"时打的那句
> STATUS，说的也是上面这层关系 —— **不额外分发一个 GPL 程序 ≠ 分发物只有 MIT 组件**。
> 修改其中任何一处措辞时请同步另一处，别再让两处说法打架。

### 8.5 换包 / 升级的操作步骤

1. 在 <https://github.com/BtbN/FFmpeg-Builds/releases> 挑一个 **autobuild** tag。
   不要钉 rolling 的 `latest` —— 它下同名资产每天被覆盖，钉死 sha256 会天天校验失败；
   月末那批是长期保留的，优先选它们。
2. 改 `cmake/ffmpeg-version.json`：
   `windows.url` / `windows.sha256` / `windows.releaseTag` / `windows.buildId`，
   以及 `version`、`license.*`。**sha256 从同 release 的 `checksums.sha256` 取，别手抄。**
3. `powershell -ExecutionPolicy Bypass -File scripts\fetch-ffmpeg.ps1 -Force`
   （`-Version` 已废弃：资产名带 commit hash，拼不出来）。
4. 复核确实是 LGPL 构建：`ffmpeg.exe -version` 的配置里必须有 `--disable-libx264`。
   顺带确认 `bin/` 里的 DLL 主版本号没变（跨大版本要重新验证 ABI）。
5. 跑一次 `cmake --install`，确认 `doc/licenses/` 下文件齐全。
