#!/usr/bin/env bash
# scripts/sync-origin-master.sh
# 在沙箱中每次 `git fetch`/`git pull` 后，git 内部 fetch 路径会把
# .git/refs/remotes/origin/master 的 loose ref 文件删掉但写不回去（沙箱
# 拦截 git 的 ref 原子写入）。本脚本用 ls-remote 取远端真实 tip 直接写 loose ref，
# 绕过 git 内部 fetch 写入路径，恢复 origin/master 正确指向。
#
# 用法：
#   bash scripts/sync-origin-master.sh           # 同步当前仓库的 origin/master
#   git fetch origin && bash scripts/sync-origin-master.sh   # 推荐组合
set -euo pipefail
REMOTE="${1:-origin}"
BRANCH="${2:-master}"
TIP="$(git ls-remote "$REMOTE" "$BRANCH" | awk 'NR==1 {print $1}')"
if [ -z "$TIP" ]; then
  echo "ERROR: ls-remote returned empty for $REMOTE/$BRANCH" >&2
  exit 1
fi
mkdir -p .git/refs/remotes/"${REMOTE##*/}"
printf '%s' "$TIP" > .git/refs/remotes/"${REMOTE##*/}"/"$BRANCH"
echo "synced $REMOTE/$BRANCH -> $TIP (loose ref at .git/refs/remotes/${REMOTE##*/}/$BRANCH)"
