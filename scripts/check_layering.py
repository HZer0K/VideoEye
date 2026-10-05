"""检查分层依赖方向有没有被违反。

规则来自 docs/ARCHITECTURE.md：
  domain        不能 include core/analysis、core/media、core/player、core/qc、
                core/reporting、FFmpeg
  infrastructure 不能 include core/*（只能靠 stdlib）
  ffmpeg_io     叶子模块: 不能 include 任何 core/* 与 infrastructure/*
                （只靠 FFmpeg 公共头，analysis/playback/exporter 共享它）
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
    # infrastructure 可以依赖 core/domain, 但不能碰 core 里的业务层。
    #
    # 这条例外是这次改出来的(2026-10-05): 后台任务的协议(TaskId / TaskState / TaskKind /
    # CancelToken / TaskHandle)原先定义在 infrastructure/concurrency/TaskManager.h 里,
    # 可分层规则只允许 infrastructure 单向依赖 core/* —— 于是 QcRunner(core/qc)、
    # QtAnalysisController(core/qt)这两处最该接入统一协议的地方**看不见**这个协议。
    # 把协议下沉到 core/domain/task 之后, 方向就闭合了: 协议在叶子层, 谁都能用;
    # infrastructure 只保留调度实现。所以禁区里去掉 core/domain, 其余业务层照旧。
    "infrastructure": ["core/analysis", "core/media", "core/player", "core/qc",
                       "core/reporting", "core/ffmpeg", "core/qt"],
    # 叶子模块: 只依赖 FFmpeg 公共头。这里逐层列名而不是写 "core/" ——
    # 模块自己的头文件也长着 "core/..." 前缀，写 "core/" 会把自引用算成违规。
    "core/ffmpeg_io": ["core/domain", "core/media", "core/analysis", "core/player",
                       "core/qc", "core/reporting", "core/ffmpeg", "core/qt",
                       "infrastructure/"],
    "core/media": ["core/analysis", "core/player", "core/qc", "core/reporting",
                   "core/ffmpeg", "core/qt"],
    "core/analysis": ["core/player", "core/qc", "core/reporting", "core/ffmpeg", "core/qt"],
    # 不需要把 infrastructure/ 列进禁区: 其它 core 层都没列, 本来就允许依赖它 ——
    # core/qc 既用 infrastructure/logging, 也用 infrastructure/concurrency(TaskManager)。
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
#
# 现在是空的 —— 两条历史例外已经各自解决：
#   * QtAnalysisController.h（别名层，反向 include core/qt）已删除；
#   * domain 直接复用 media 的 NalUnit / ObuUnit 已改为 domain 自有结果类型
#     core/domain/model/BitstreamUnits.h，由 BitstreamAnalyzer 做 media -> domain 转换。
#
# 别往里加新条目：这里的每一行都是"依赖方向没闭合"的欠条，迁移完就该删掉。
EXCEPTIONS = set()

def matches_banned(include, banned):
    """include 是否落在禁区的**目录**里。

    必须按路径段比较，不能裸 startswith: 禁 "core/ffmpeg" 时
    "core/ffmpeg_io/xxx.h" 会被朴素前缀匹配误判成违规 —— 而这是两个不相干的层。
    """
    for entry in banned:
        prefix = entry.rstrip("/")
        if include == prefix or include.startswith(prefix + "/"):
            return True
    return False


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
            if matches_banned(inc, banned):
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
