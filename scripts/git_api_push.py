#!/usr/bin/env python3
"""Push commits through the GitHub Git Data API.

Use when the plain `git push` RPC is torn down by a MITM proxy
(curl 52 / curl 16 / TLS unexpected eof) but api.github.com is reachable.

The commits are rebuilt byte-identically on the remote side: the same tree
entries, author/committer identity and timestamps are sent, so the resulting
commit sha must match the local one. Every step asserts that equality and
aborts otherwise -- nothing is updated on the remote unless it matches.

Usage:
  python scripts/git_api_push.py [--repo owner/name] [--range BASE..HEAD]
                                 [--ref refs/heads/master] [--dry-run]
"""

from __future__ import annotations

import argparse
import base64
import datetime
import json
import os
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request

API = "https://api.github.com"


class ApiError(RuntimeError):
    pass


def git(*args, binary=False, stdin_data=b""):
    # stdin 必须显式给掉。subprocess.run 默认继承父进程的 stdin，而
    # `git credential fill` 是先读 stdin 上一段 protocol/host 描述才应答的
    # —— 从终端跑时父进程 stdin 就是键盘，读不到 EOF 就一直等，
    # 表现为「脚本刚启动就卡住，没有任何输出」（实测挂了 28 分钟）。
    # 这里一律传 input=，其余子命令拿到空 stdin 行为不变。
    p = subprocess.run(["git", *args], input=stdin_data, capture_output=True, check=True)
    return p.stdout if binary else p.stdout.decode("utf-8", "replace")


def get_credential_token(token_file: str | None) -> str:
    if token_file:
        with open(token_file, "r", encoding="utf-8") as fh:
            token = fh.read().strip()
        if token:
            return token
    out = git("credential", "fill", stdin_data=b"protocol=https\nhost=github.com\n\n")
    token = None
    for line in out.splitlines():
        if line.startswith("password="):
            token = line[len("password="):]
    if not token:
        raise RuntimeError("no https credential stored for github.com")
    return token.strip()


def build_opener(token: str):
    ctx = ssl._create_unverified_context()
    proxies = {}
    for name in ("https_proxy", "HTTPS_PROXY", "http_proxy", "HTTP_PROXY"):
        val = os.environ.get(name)
        if val:
            proxies["https"] = val
            proxies["http"] = val
            break
    handler = urllib.request.HTTPSHandler(context=ctx)
    if proxies:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler(proxies), handler)
    else:
        opener = urllib.request.build_opener(handler)
    opener.addheaders = [
        ("Authorization", f"Bearer {token}"),
        ("Accept", "application/vnd.github+json"),
        ("User-Agent", "videoeye-git-api-push"),
        ("X-GitHub-Api-Version", "2022-11-28"),
    ]
    return opener


def warmup(opener) -> None:
    """Probe the API first: the proxy drops the first requests of a session."""
    for _ in range(5):
        try:
            request(opener, "GET", "/rate_limit", retries=1)
            return
        except ApiError:
            time.sleep(3)


def request(opener, method: str, path: str, payload=None, retries: int = 10):
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    last = None
    for attempt in range(retries):
        req = urllib.request.Request(API + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with opener.open(req, timeout=180) as resp:
                raw = resp.read()
                return json.loads(raw) if raw else {}
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")
            if e.code in (400, 429, 500, 502, 503, 504) and attempt < retries - 1:
                # the proxy mangles the first requests of a session; retry
                last = f"{e.code}: {body[:200]}"
                time.sleep(3)
                continue
            raise ApiError(f"{method} {path} -> {e.code}: {body[:800]}") from None
        except (urllib.error.URLError, OSError) as e:
            last = e
            time.sleep(3)
    raise ApiError(f"{method} {path} -> {last}") from None


def iso8601(ts: int, tz: str) -> str:
    sign = 1 if tz[0] == "+" else -1
    offset = datetime.timedelta(hours=int(tz[1:3]), minutes=int(tz[3:5])) * sign
    dt = datetime.datetime.fromtimestamp(ts, tz=datetime.timezone(offset))
    return dt.isoformat()


def split_ident(line: str):
    # "author xiaokang <2395711943@qq.com> 1792421314 +0800"
    kind, rest = line.split(" ", 1)
    email_start = rest.rindex("<")
    email_end = rest.index(">", email_start)
    name = rest[:email_start].strip()
    email = rest[email_start + 1:email_end]
    tail = rest[email_end + 1:].split()
    date = iso8601(int(tail[0]), tail[1])
    return kind, {"name": name, "email": email, "date": date}


def parse_commit(sha: str):
    raw = git("cat-file", "commit", sha, binary=True)
    head, _, message = raw.partition(b"\n\n")
    tree = None
    parents = []
    author = None
    committer = None
    for line in head.decode("utf-8").splitlines():
        if line.startswith("tree "):
            tree = line[5:].strip()
        elif line.startswith("parent "):
            parents.append(line[7:].strip())
        elif line.startswith("author "):
            author = split_ident(line)[1]
        elif line.startswith("committer "):
            committer = split_ident(line)[1]
        elif line.startswith("gpgsig"):
            raise RuntimeError(f"{sha} is GPG signed; API push cannot reproduce it")
    return {
        "tree": tree,
        "parents": parents,
        "author": author,
        "committer": committer,
        "message": message.decode("utf-8"),
    }


def changed_entries(parent: str, commit: str, opener, repo: str):
    lines = git("-c", "core.quotepath=false", "diff-tree", "-r", "--no-renames",
                "--no-commit-id", parent, commit).split("\n")
    entries = []
    for line in lines:
        if not line.strip():
            continue
        meta, _, path = line.partition("\t")
        # meta example: ":100644 100644 <src> <dst> M"
        fields = meta.split()
        src_mode, dst_mode, src_sha, dst_sha, status = (
            fields[0][1:], fields[1], fields[2], fields[3], fields[4])
        if status == "D":
            entries.append({"path": path, "mode": src_mode, "type": "blob", "sha": None})
            continue
        if dst_mode not in ("100644", "100755"):
            raise RuntimeError(f"unsupported mode {dst_mode} for {path}")
        # always upload the raw blob: base64 survives any encoding, and it keeps
        # the tree request small (inlining content hit 400s on larger payloads)
        blob = git("cat-file", "blob", dst_sha, binary=True)
        created = request(opener, "POST", f"/repos/{repo}/git/blobs",
                          {"content": base64.b64encode(blob).decode(),
                           "encoding": "base64"})
        if created["sha"] != dst_sha:
            raise RuntimeError(f"blob mismatch for {path}: "
                               f"remote {created['sha']} != local {dst_sha}")
        entries.append({"path": path, "mode": dst_mode, "type": "blob",
                        "sha": dst_sha})
    return entries


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default="HZer0K/VideoEye")
    ap.add_argument("--range", default="origin/master..HEAD")
    ap.add_argument("--ref", default="refs/heads/master")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--token-file",
                    help="file holding the PAT; avoids spawning the git credential helper")
    args = ap.parse_args()

    revs = git("rev-list", "--reverse", args.range).split()
    if not revs:
        print("nothing to push")
        return 0
    print(f"commits to push: {revs}")

    token = get_credential_token(args.token_file)
    opener = build_opener(token)
    warmup(opener)

    # parent of the first commit is the current remote tip
    remote_parent = git("rev-parse", revs[0] + "^").strip()
    tip = remote_parent
    parent_tree = git("rev-parse", remote_parent + "^{tree}").strip()
    print(f"remote base: {tip[:12]} tree {parent_tree[:12]}")

    for sha in revs:
        info = parse_commit(sha)
        entries = changed_entries(info["parents"][0], sha, opener, args.repo)
        print(f"  + {sha[:12]} {info['message'].splitlines()[0]} "
              f"({len(entries)} files)")

        if args.dry_run:
            continue

        tree = request(opener, "POST", f"/repos/{args.repo}/git/trees",
                       {"base_tree": parent_tree, "tree": entries})
        if tree["sha"] != info["tree"]:
            raise RuntimeError(f"tree mismatch for {sha}: "
                               f"remote {tree['sha']} != local {info['tree']}")
        commit = request(opener, "POST", f"/repos/{args.repo}/git/commits",
                         {"message": info["message"],
                          "tree": info["tree"],
                          "parents": info["parents"],
                          "author": info["author"],
                          "committer": info["committer"]})
        if commit["sha"] != sha:
            raise RuntimeError(f"commit mismatch: remote {commit['sha']} != local {sha}")
        tip = sha
        parent_tree = info["tree"]

    if args.dry_run:
        print("dry-run: nothing sent")
        return 0

    request(opener, "PATCH", f"/repos/{args.repo}/git/{args.ref}", {"sha": tip})
    print(f"updated {args.ref} -> {tip}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ApiError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
