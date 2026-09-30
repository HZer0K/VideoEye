"""检查分层依赖方向有没有被违反。

规则来自 docs/ARCHITECTURE.md：
  domain        不能 include core/analysis、core/media、core/player、core/qc、
                core/reporting、FFmpeg
  infrastructure 不能 include core/*（只能靠 stdlib）
  media         不能 include core/analysis、core/player、core/qc、core/reporting
  analysis      不能 include core/player、core/qc、core/reporting
  qc            不能 include core/player、core/reporting
  reporting     不能 include core/player、core/analysis（遗留 ReportExporter 除外）
  ui            可以 include 一切（界面本来就横跨多层）
"""
import os
import re

ROOT = os.environ.get("VIDEOEYE_ROOT") or os.getcwd()
INC = re.compile(r'#\s*include\s+"(core/[^"]+|infrastructure/[^"]+|utils/[^"]+)"')
FFMPEG = re.compile(r'#\s*include\s*<(libav|libsw)')

RULES = {
    "core/domain": ["core/analysis", "core/media", "core/player", "core/qc",
                    "core/reporting", "core/ffmpeg", "core/qt"],
    "infrastructure": ["core/"],
    "core/media": ["core/analysis", "core/player", "core/qc", "core/reporting",
                   "core/ffmpeg", "core/qt"],
    "core/analysis": ["core/player", "core/qc", "core/reporting", "core/ffmpeg", "core/qt"],
    "core/qc": ["core/player", "core/reporting"],
    "core/reporting": ["core/player", "core/analysis", "core/qt"],
    "core/ffmpeg": ["core/analysis", "core/player", "core/qc"],
    "core/qt": ["core/player", "core/qc", "core/reporting"],
}
NO_FFMPEG = ["core/domain", "infrastructure", "core/media", "core/qc", "core/reporting",
             "core/ffmpeg"]


def walk(d):
    for cur, _, files in os.walk(os.path.join(ROOT, d)):
        for name in files:
            if name.endswith((".h", ".cpp")):
                yield os.path.join(cur, name)


# 白名单: 迁移期明确允许的例外（都写进了 docs/ARCHITECTURE.md 的"已知历史包袱"）
# 迁移期允许的两条反向 include。改完就该从这里删掉，别往里加。
EXCEPTIONS = {
    # AnalysisCoordinator.h 现在只是一层别名（= QtAnalysisController），保留给旧调用方。
    ("core/analysis/orchestration/AnalysisCoordinator.h", "core/qt/QtAnalysisController.h"),
    # BitstreamInfo 直接复用码流层的 NalUnit / ObuUnit 结果类型（下放到 domain 前的过渡）。
    ("core/domain/model/BitstreamInfo.h", "core/media/codec/ExtradataTypes.h"),
}

violations = []
for layer, banned in RULES.items():
    for path in walk(layer):
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        text = open(path, encoding="utf-8", errors="ignore").read()
        # 去掉注释行，避免"文档里提到"被算成依赖
        body = re.sub(r"//[^\n]*", "", text)
        for inc in INC.findall(body):
            if (rel, inc) in EXCEPTIONS:
                continue
            if any(inc.startswith(b) for b in banned):
                violations.append((layer, rel, "include " + inc))
        if layer in NO_FFMPEG and FFMPEG.search(body):
            violations.append((layer, rel, "include FFmpeg 头文件"))

if violations:
    print("[!] 违反依赖方向:")
    for layer, rel, why in sorted(violations):
        print("   [%-16s] %-52s %s" % (layer, rel, why))
else:
    print("OK: 没有跨层反向依赖")
print("检查了 %d 个目录层" % len(RULES))
