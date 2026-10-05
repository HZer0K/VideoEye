"""检查「命名空间有没有跟着目录走」。

规则来自 docs/ARCHITECTURE.md §5：目录（以及 CMake target）已经分层了，命名空间
也必须跟着走。2026-10-05 清掉的两条历史包袱：

  * ``videoeye::analyzer`` —— 对应 core/analysis/ 的那一层，拍平后就是 ``videoeye``；
  * ``videoeye::utils`` —— utils/ 目录当年拆进 core/media/** 与 infrastructure/**，
    拍平后也是 ``videoeye``（core/media/streaming 里原有的 utils::manifest 顺势
    落成 ``videoeye::manifest``，它本来就是「跟目录走」的样板）。

三条规则：

  R1 不得再声明 ``namespace analyzer`` / ``namespace utils``（回归守卫）；
  R2 不得再出现 ``videoeye::analyzer::`` / ``analyzer::`` / ``utils::`` 形式的引用；
  R3 videoeye 下的具名子命名空间必须与它所在文件的某个上级目录同名。
    已知偏差列在 KNOWN_DEVIATIONS 里并写明理由；**新出现的偏差直接判违规** ——
    这条规则的价值就在于「以后再冒出一个没登记的偏差就红」。

退出码：有违规 = 1，全过 = 0（pre-commit / CI / ctest 都靠这个退出码把关）。
"""
import os
import re
import sys

ROOT = os.environ.get("VIDEOEYE_ROOT") or os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# .workbuddy 里全是历史快照 / 构建日志 / 修复脚本，不是源码 —— 不排除的话
# 光 analysis_backup、ns_rebuild 两棵子树就能把审计刷出 400+ 条噪音。
SKIP_DIRS = {".git", "build", "vcpkg_installed", ".cache", "docs", "node_modules", "out",
             ".workbuddy", "scripts"}

# R3 的已知偏差：(文件路径, 命名空间, 理由)。新出现的偏差不在表里就会被判违规。
KNOWN_DEVIATIONS = [
    ("core/media/streaming/ManifestText.h", "manifest",
     "清单文本解析工具（Trim/拼接）与 ManifestReader 同属一个子命名空间，"
     "2026-10-05 从 utils::manifest 拍平而来；产出的是 videoeye::manifest，"
     "调用方一律写 mt::X（namespace mt = videoeye::manifest），不再有歧义。"),
    ("core/media/streaming/ManifestReader.h", "manifest",
     "同 ManifestText.h —— manifest 是 streaming 这一层的对外子命名空间。"),
    # 下面是 2026-10-05 顺手被 R3 抓出来的**在册偏差**。本轮只清 analyzer / utils 两条
    # （评审清单里点名的 85 处），这三条动到 130+ 处调用点，放到下一轮单独做。
    # 登记的意义是：它们已经是「明文列出来、有人认领」的债，不再是看不见的漂移；
    # 而且只要再冒出一个没登记的偏差，脚本照样红。
    # 匹配写法：路径以 / 结尾按目录前缀匹配，否则按文件前缀匹配。
    ("core/ffmpeg/", "ffmpegtool",
     "core/ffmpeg/ 整层都叫 videoeye::ffmpegtool（10 个文件，130+ 处引用）。"
     "改名到 videoeye::ffmpeg 是纯机械替换，但要连带改别名 "
     "namespace ffmpegtool = videoeye::ffmpegtool;，且 core/ffmpeg_io 那边是 "
     "videoeye::ffmpeg_io，别混。"),
    ("core/player/FrameData.", "model",
     "FrameData 是 videoeye::model 的类型，文件却躺在 core/player/ 下。"
     "要么把文件搬去 core/domain/model/（真正的「跟目录走」），要么在 R3 里给它开一条"
     "正例豁免。2026-10-05 先登记不搬 —— 搬文件要同步改 include 链路与 CMake GLOB。"),
    ("infrastructure/concurrency/TaskManager.", "task",
     "任务协议（TaskId / TaskState / TaskKind / CancelToken / TaskHandle）2026-10-05 "
     "已下沉到 core/domain/task，namespace task 是**有意**保留的子命名空间："
     "infrastructure/concurrency 只放调度实现。这属于「协议与实现分家」，不是漂移。"),
]

NAMESPACE_RE = re.compile(r'^\s*namespace\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\{')
VIDEOEYE_OPEN = re.compile(r'^\s*namespace\s+videoeye\s*\{')
BANNED_NS = ("analyzer", "utils")
BANNED_REF = [
    (re.compile(r'\bvideoeye::analyzer::'), "videoeye::analyzer::"),
    (re.compile(r'(?<![A-Za-z0-9_])analyzer::'), "analyzer::"),
    (re.compile(r'\bvideoeye::utils::'), "videoeye::utils::"),
    (re.compile(r'(?<![A-Za-z0-9_])utils::'), "utils::"),
]


def iter_sources():
    for cur, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in files:
            if name.endswith((".h", ".cpp")):
                yield os.path.join(cur, name)


def first_sub_namespace(text):
    """返回文件里 `namespace videoeye {` 之后紧跟的第一个具名子命名空间；没有则返回 None。"""
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if not VIDEOEYE_OPEN.match(line):
            continue
        for j in range(i + 1, min(i + 4, len(lines))):
            m = NAMESPACE_RE.match(lines[j])
            if m:
                return m.group("name")
            if lines[j].strip() and not lines[j].strip().startswith("//"):
                break
        return None
    return None


def parent_dirs(rel):
    parts = rel.split("/")
    return [p for p in parts[:-1] if p]


def main():
    violations = []
    deviations = []
    checked = 0

    known = [(r, n) for r, n, _ in KNOWN_DEVIATIONS]

    for path in iter_sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        try:
            text = open(path, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        checked += 1

        # R1 / R2: 注释里提到不算，先剥掉 // 行注释。
        body = re.sub(r"//[^\n]*", "", text)

        for ns in BANNED_NS:
            if re.search(r'^\s*namespace\s+' + ns + r'\s*\{', body, re.M):
                violations.append((rel, "R1 声明了 namespace %s（历史包袱已清，不要再引入）" % ns))

        for pattern, label in BANNED_REF:
            m = pattern.search(body)
            if m:
                line_no = text[:m.start()].count("\n") + 1
                violations.append((rel, "R2 引用 %s（第 %d 行）" % (label, line_no)))

        # R3: 子命名空间跟目录一致
        ns = first_sub_namespace(text)
        if not ns:
            continue
        dirs = parent_dirs(rel)
        # 目录名 + 上层目录名都算命中（core/media/codec/X.h -> codec / media）
        if ns in dirs[:-1] or ns in dirs[-1:]:
            continue
        if any(r == rel or (r.endswith("/") and rel.startswith(r)) or rel.startswith(r)
               for r, n in known if n == ns):
            deviations.append((rel, ns))
            continue
        violations.append((rel, "R3 子命名空间 videoeye::%s 与目录 %s 不一致" % (ns, "/".join(dirs[-2:] or dirs))))

    if violations:
        print("[!] 命名空间布局违规:")
        for rel, why in sorted(violations):
            print("   %-56s %s" % (rel, why))
        print("\n共 %d 处。修掉之后重跑: python scripts/audit_namespace_layout.py" % len(violations))
        print("[退出码] 1 —— 命名空间没跟着目录走，提交/流水线会被拦下")
        sys.exit(1)

    print("OK: 命名空间与目录一致")
    print("检查了 %d 个源文件，%d / %d 条已知偏差复核通过"
          % (checked, len(deviations), len(KNOWN_DEVIATIONS)))
    for rel, ns in sorted(deviations):
        print("   [已知偏差] %-52s videoeye::%s" % (rel, ns))


if __name__ == "__main__":
    main()
