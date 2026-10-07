#!/bin/bash
# 安装 git hooks 到 .git/hooks/
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
HOOKS_DIR="$ROOT/.git/hooks"

if [ ! -d "$HOOKS_DIR" ]; then
    echo "错误: 未找到 .git/hooks 目录 ($HOOKS_DIR)，请在仓库根目录的 git 仓库中运行"
    exit 1
fi

cp "$DIR/git-hooks/pre-commit" "$HOOKS_DIR/pre-commit"
chmod +x "$HOOKS_DIR/pre-commit"
echo "pre-commit hook 已安装到 $HOOKS_DIR/pre-commit"
echo ""
echo "hook 功能（按顺序）:"
echo "  1. 分层边界检查 scripts/check_layering.py —— 跨层反向依赖直接阻止提交"
echo "  2. Qt/domain 边界审计 scripts/audit_qt_domain_border.py —— 确定命中直接阻止提交"
echo "  3. 审计退出码自测 scripts/test_audit_qt_domain_border.py —— 保证上面那道门真拦得住"
echo "  4. 其余架构审计 + 各自退出码自测 (Qt/analysis 边界 / 命名空间 / 链接可见性)"
echo "  5. 暂存的 C++ 文件跑 clang-format --dry-run --Werror"
echo ""
echo "与 CI 的关系: .github/workflows/build.yml 的 layering job 跑同样的脚本，"
echo "本地 hook 只是为了别等到 CI 红 —— CMake 拦不住跨层 include，详见该脚本注释。"
echo "第 5 道门需要 clang-format；没装时只 warning 跳过（CI 有独立的 format job 兜底）。"
echo "卸载: rm $HOOKS_DIR/pre-commit"
