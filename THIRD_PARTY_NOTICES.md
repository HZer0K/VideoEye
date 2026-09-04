# 开源协议与第三方声明 (Open Source Licenses & Third-Party Notices)

## 本项目的许可 (VideoEye)

VideoEye 是一个基于雷霄骅(Lei Xiaohua)原版思路重新实现的现代化视频分析软件，
仓库中由本团队原创的源码采用 **双许可**：

- **MIT**：见 [`LICENSE-MIT`](LICENSE-MIT)。纯 VideoEye 原创代码（不链接下述
  GPL 组件的部分）可按 MIT 使用。
- **GPL-3.0-or-later**：见 [`LICENSE`](LICENSE)。

**为何整体分发按 GPL？** 程序静态链接了 `third_party/Bento4`（ap4，**GPLv2+
许可**，见 `third_party/Bento4/Documents/LICENSE.txt`）。GPL 对组合程序具有传染性，
因此**以可执行程序/二进制形式对外分发时，整体必须以 GPL-2.0-or-later 或
GPL-3.0-or-later 授权**（Bento4 源码声明 "version 2, or any later version"）。
本项目整体采用 **GPL-3.0-or-later**；若要按纯 MIT 分发，请取得 Bento4 商业授权
或移除/替换 Bento4 依赖。

## 第三方组件与许可一览

| 组件 | 目录 | 许可 | 使用方式 | 义务 |
|---|---|---|---|---|
| Bento4 (ap4) | `third_party/Bento4` | GPL-2.0-or-later | 静态链接 | 整体按 GPL 分发 |
| MediaInfoLib | `third_party/MediaInfoLib` | BSD-2-Clause | 静态链接 | 二进制分发须复现版权声明（见下） |
| ZenLib | `third_party/ZenLib` | zlib 许可 | 静态链接 | 保留声明 |
| Vulkan-Headers | `third_party/vulkan-headers` | Apache-2.0 | 头文件 | 保留声明 |
| FFmpeg | 外部动态链接 | LGPL-2.1-or-later | 动态链接 | 用户可替换库（未修改 FFmpeg 源码） |
| Qt 6 | 外部动态链接 | LGPL-3.0 / GPL-3.0 / 商业 | 动态链接 | 用户可替换库（未修改 Qt） |
| SDL2 | 外部动态链接 | zlib | 动态链接 | 保留声明 |

> 各组件完整的许可全文以其目录内 `LICENSE` / `License.txt` / `COPYING` 等文件为准。

## 必需的通知文本 (Required Notices)

### MediaInfoLib — BSD 2-Clause

```
Copyright (c) 2002-2025, MediaArea.net SARL
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### ZenLib — zlib 许可

```
Copyright (c) 2002-2025 MediaArea.net SARL. All rights reserved.

This software is provided 'as-is', without any express or implied warranty.
Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it freely,
subject to the following restrictions:
1. The origin of this software must not be misrepresented.
2. Altered source versions must be plainly marked as such.
3. This notice may not be removed or altered from any source distribution.
```

### SDL2 — zlib 许可

```
This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it freely,
subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### Vulkan-Headers — Apache-2.0

```
Copyright 2015-2023 The Khronos Group Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```
