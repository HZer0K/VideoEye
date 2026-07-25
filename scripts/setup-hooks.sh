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
echo "hook 功能: 提交前对暂存的 C++ 文件运行 clang-format 检查"
echo "卸载: rm $HOOKS_DIR/pre-commit"
