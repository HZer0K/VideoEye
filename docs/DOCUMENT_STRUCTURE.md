# VideoEye 文档组织结构

本文档说明 VideoEye 项目的完整文档组织结构，包括公开文档和本地文档的分类和管理规则。

## 📁 最终文档结构

```
VideoEye/
├── README.md                          # 项目说明（公开）
├── QUICKSTART.md                      # 快速入门（公开）
├── docs/                              # 文档根目录
│   ├── [功能技术文档]                  # 公开文档 ⭐
│   │   ├── BITSTREAM_ANALYSIS.md     # Bitstream 分析技术文档
│   │   ├── AUDIO_QC.md               # Audio QC 功能文档
│   │   ├── BITRATE_GOP_ANALYSIS.md   # Bitrate GOP 分析文档
│   │   ├── COLOR_HDR_ANALYSIS.md     # Color HDR 分析文档
│   │   ├── DIAGNOSTICS_QC.md         # Diagnostics QC 功能文档
│   │   └── MP4_SAMPLE_TABLE.md       # MP4 Sample Table 技术文档
│   │
│   ├── bitstream/                     # Bitstream 实施文档（本地）🔒
│   │   ├── README.md                 # 文档索引
│   │   ├── BITSTREAM_IMPLEMENTATION_STATUS.md
│   │   ├── BITSTREAM_SUMMARY.md
│   │   ├── README_BITSTREAM.md
│   │   ├── IMPLEMENTATION_COMPLETE.md
│   │   └── FILE_LIST.md
│   │
│   └── local/                         # 本地开发文档（本地）🔒
│       ├── README.md                 # 文档索引
│       ├── ARCHITECTURE.md           # 系统架构设计
│       ├── PROJECT_STRUCTURE.md      # 项目结构说明
│       ├── VULKAN_OPTIMIZATION_PLAN.md # Vulkan 优化规划
│       └── DOCUMENT_ORGANIZATION.md  # 文档组织说明
│
└── .gitignore                         # Git 忽略配置
    └── 忽略：docs/bitstream/, docs/local/
```

## 📊 文档分类矩阵

| 文档类型 | 位置 | 是否公开 | 用途 | 示例 |
|---------|------|---------|------|------|
| **项目说明** | 根目录 | ✅ 是 | 用户入门 | README.md, QUICKSTART.md |
| **功能技术文档** | docs/ | ✅ 是 | 功能说明 | BITSTREAM_ANALYSIS.md, AUDIO_QC.md |
| **实施文档** | docs/bitstream/ | ❌ 否 | 实施细节 | BITSTREAM_*.md |
| **架构文档** | docs/local/ | ❌ 否 | 内部设计 | ARCHITECTURE.md |
| **优化规划** | docs/local/ | ❌ 否 | 未来规划 | VULKAN_OPTIMIZATION_PLAN.md |

## 🔍 文档用途说明

### 1. 公开文档（docs/ 根目录）

**特点**: 
- ✅ 推送到远程仓库
- ✅ 对外公开
- ✅ 面向用户和贡献者

**内容**:
- 具体功能的**技术原理和使用方法**
- API 文档、数据结构说明
- 编码格式解析技术细节

**示例**:
- `BITSTREAM_ANALYSIS.md` - H.264/H.265/AV1/VVC 码流解析技术
- `AUDIO_QC.md` - 音频质量分析功能
- `MP4_SAMPLE_TABLE.md` - MP4 容器结构技术

### 2. 实施文档（docs/bitstream/）

**特点**:
- 🔒 仅保存在本地
- 🔒 不推送到远程仓库
- 🔒 面向开发团队

**内容**:
- 功能实现的**详细过程记录**
- 测试报告、状态跟踪
- 文件清单、统计信息

**示例**:
- `BITSTREAM_IMPLEMENTATION_STATUS.md` - Bitstream 功能实施状态
- `FILE_LIST.md` - 所有实现文件的详细列表

### 3. 本地开发文档（docs/local/）

**特点**:
- 🔒 仅保存在本地
- 🔒 不推送到远程仓库
- 🔒 面向核心开发团队

**内容**:
- **架构设计和决策**
- **项目演进历史**
- **未来优化规划**

**示例**:
- `ARCHITECTURE.md` - 系统整体架构设计
- `VULKAN_OPTIMIZATION_PLAN.md` - Vulkan 深化应用路线图

## 🎯 使用场景指南

### 对于新用户/贡献者

**你应该看**:
1. `README.md` - 了解项目是什么
2. `QUICKSTART.md` - 快速上手
3. `docs/BITSTREAM_ANALYSIS.md` - 学习具体功能技术
4. `docs/AUDIO_QC.md` - 了解其他功能

**你不应该看**（或无法访问）:
- `docs/local/` 下的文档 - 这些是内部文档
- `docs/bitstream/` 下的文档 - 这些是实施细节

### 对于团队成员

**你可以看**:
1. 所有公开文档
2. `docs/local/` 下的文档 - 了解架构和设计
3. `docs/bitstream/` 下的文档 - 了解实施细节

**建议阅读顺序**:
```
新手入门流程:
1. README.md
2. QUICKSTART.md
3. docs/local/ARCHITECTURE.md
4. docs/local/PROJECT_STRUCTURE.md
5. 根据需要查看具体功能文档

功能开发流程:
1. docs/BITSTREAM_ANALYSIS.md (技术原理)
2. docs/bitstream/README.md (实施文档索引)
3. 根据需求查看具体实施文档
```

### 对于维护者

**管理职责**:
1. 决定哪些文档应该公开（放在 docs/ 根目录）
2. 哪些文档应该本地化（移到 docs/local/ 或 docs/bitstream/）
3. 更新 .gitignore 确保本地文档不被推送

## 🔒 Git 配置

`.gitignore` 中的关键配置：

```gitignore
# 本地实施文档（不推送到远程仓库）
docs/bitstream/

# 本地开发文档（不推送到远程仓库）
docs/local/
```

**效果**:
- ✅ `docs/` 根目录的公开文档会正常推送
- ❌ `docs/bitstream/` 和 `docs/local/` 完全本地化
- 🔐 保护项目内部信息和未来规划

## 📈 文档创建最佳实践

### 何时创建公开文档？

当文档包含：
- ✅ 用户需要了解的技术原理
- ✅ API 使用说明
- ✅ 数据格式定义
- ✅ 功能特性说明

**例子**: `BITSTREAM_ANALYSIS.md`

### 何时创建本地文档？

当文档包含：
- 🔒 项目架构设计
- 🔒 实施过程和状态跟踪
- 🔒 未来规划和路线图
- 🔒 内部决策记录

**例子**: `ARCHITECTURE.md`, `BITSTREAM_IMPLEMENTATION_STATUS.md`

## 📊 统计信息

截至 2026-09-19：

| 类别 | 文件数 | 总行数 | 位置 |
|------|--------|--------|------|
| **公开文档** | 8 | ~80K | 根目录 + docs/ |
| **Bitstream 实施** | 6 | ~3.2K | docs/bitstream/ |
| **本地开发** | 4 | ~8K | docs/local/ |
| **总计** | 18 | ~91K | - |

## 🚀 快速导航

- 🏠 **项目首页**: `README.md`
- 🚀 **快速开始**: `QUICKSTART.md`
- 📖 **技术文档**: `docs/BITSTREAM_ANALYSIS.md`
- 🔧 **架构设计**: `docs/local/ARCHITECTURE.md`
- 📋 **Bitstream 文档**: `docs/bitstream/README.md`
- 📂 **本地文档**: `docs/local/README.md`

---

**最后更新**: 2026-09-19  
**维护者**: VideoEye Team  
**版本**: v1.0
