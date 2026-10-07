#!/usr/bin/env bash
# 列出「本次改动/新增的 C++ 文件」——给 CI 的增量静态检查门禁（格式检查 / clang-tidy
# 摸底）限定范围。全仓历史文件从未做过全量格式化，一次性卡全量会把无关旧文件卷进
# 同一个红；增量语义与 pre-commit 钩子的第 4 道门一致（暂存文件）。
#
# 用法:
#   bash scripts/ci-changed-files.sh                # 按 GitHub Actions 上下文自动找基线
#   bash scripts/ci-changed-files.sh <基线ref|sha>  # 显式指定基线（本地排查用）
#
# 基线规则:
#   pull_request: 基线分支与 HEAD 的 merge-base;
#   push:          github.event.before（首次推送 / 强推时回退 HEAD~1）;
#   都拿不到:      输出为空并退出 0 —— 宁可跳过本次增量检查，也不要拿错基线把门打红。
set -euo pipefail

base="${1:-}"

if [ -z "$base" ]; then
    if [ "${GITHUB_EVENT_NAME:-}" = "pull_request" ]; then
        base="origin/${GITHUB_BASE_REF:?pull_request 事件缺少 GITHUB_BASE_REF}"
        if ! git rev-parse --verify --quiet "$base" >/dev/null; then
            git fetch --no-tags origin "$GITHUB_BASE_REF" >/dev/null 2>&1 || true
            base=FETCH_HEAD
        fi
        if git rev-parse --verify --quiet "$base" >/dev/null; then
            base=$(git merge-base "$base" HEAD)
        else
            base=""
        fi
    else
        base="${GITHUB_EVENT_BEFORE:-}"
        if [ -z "$base" ] || [ "$base" = "0000000000000000000000000000000000000000" ] ||
            ! git cat-file -e "$base^{commit}" 2>/dev/null; then
            base=$(git rev-parse HEAD~1 2>/dev/null || true)
        fi
    fi
fi

if [ -z "$base" ] || ! git cat-file -e "$base^{commit}" 2>/dev/null; then
    echo "无法确定比较基线，跳过本次增量检查" >&2
    exit 0
fi

git diff --name-only --diff-filter=ACM "$base" HEAD -- '*.c' '*.cpp' '*.h' '*.hpp'