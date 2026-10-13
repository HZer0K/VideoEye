# VideoEye

媒体分析与 QC 工具：容器结构、码流参数集、逐帧/逐包、GOP 与码率、视觉缺陷体检、字幕时码、HLS/DASH 清单分析、导出与报告。

## 下载

| 平台 | 文件 | 说明 |
|------|------|------|
| Windows x86_64 | `VideoEye-<ver>-windows-x86_64.exe` | NSIS 安装包（若本次构建未生成，说明 runner 上缺 makensis，请改用下面的 ZIP） |
| Windows x86_64 | `VideoEye-<ver>-windows-x86_64.zip` | 免安装版，解压即可运行 |
| Linux x86_64 | `VideoEye-<ver>-linux-x86_64.deb` | `sudo dpkg -i <file>`，依赖由包自动声明 |
| Linux x86_64 | `VideoEye-<ver>-linux-x86_64.tar.gz` | 免安装版 |
| 全部 | `VideoEye-<ver>-<platform>.cdx.json` | SBOM（CycloneDX 1.5），含 FFmpeg / Qt6 的版本与许可证 |
| 全部 | `SHA256SUMS.txt` | 每个产物的 SHA256（有 GPG 签名时附带 `.asc`） |

**安装前请先校验**：

```bash
sha256sum -c SHA256SUMS.txt          # Linux / macOS
CertUtil -hashfile <file> SHA256     # Windows
```

## 许可证（重要）

VideoEye **自身源码是 MIT**，但分发物里包含第三方二进制，按各自的许可证履约 ——
不要把整个发布包笼统称为"MIT 软件"。

| 组件 | 许可证 |
|------|--------|
| VideoEye 源码 | MIT |
| FFmpeg（Windows 随包） | LGPL-3.0-or-later（BtbN `win64-lgpl-shared`，不含 `libx264`/`libx265`） |
| FFmpeg（Linux，系统库） | 取决于发行版的构建配置，多数带 GPL 组件 |
| Qt 6 Widgets | LGPL-3.0-only（另有 GPL / 商业授权可选） |

每个包内都带 `doc/THIRD_PARTY_NOTICES.md`（版本、来源、许可证、对应源码获取方式）
与 `doc/licenses/`（许可证全文）。**二次分发时请一并保留这两个目录。**

导出视频时的取舍：Windows 随包的 FFmpeg 是 LGPL 构建，没有 `libx264`/`libx265`，
`MediaExporter` 会自动降级到 `libopenh264` / `mpeg4` —— 功能可用，压缩率略低。

## 本次发布验证过什么

- Windows：安装包静默安装 + 免安装版解压，均在干净路径上启动存活 8 秒
- Linux：`.deb` 用 `dpkg -i` 真装后启动存活 8 秒；tar.gz 解包后 `ldd` 无未解析依赖
- 两者都跑过完整单元测试

**未覆盖**：macOS 目前没有官方二进制。它不在常规 CI 矩阵里，也没有做干净机器安装验证 ——
在 macOS 上请自行从源码构建（`cmake --preset linux-release`）。

## 已知能力边界（不是 bug）

- **HLS / DASH 直播是"单次快照检查"**，不持续轮询播放列表。要观察窗口滚动，请手动重新分析。
- **远程清单不下载**：只分析本地可访问的清单与分片；放在远端服务器上的分片不会逐个拉取。
- **TS 分片只抽查前若干个**（默认 3 个），不是全量遍历 —— 结论不能等同于"全片无问题"。
- **VVC 是实验性部分支持**：SPS/VPS 关键字段可读，PPS 与真实样本覆盖不完整，界面上会标
  "参考"，且不参与容器一致性的严格判定。
- **码流解析按能力矩阵生效**：H.264/HEVC 只有在已验证的 Profile / 色度 / 位深组合下才判为
  "Supported"；其余组合标"部分支持"，字段仅供参考。
- 位深未解析出来时显示"位深未知"，不会拿默认值冒充 8bit。

## 反馈

问题请提在 [Issues](https://github.com/HZer0K/VideoEye/issues)，附上
`帮助 → 格式支持` 里导出的环境快照会快很多。
