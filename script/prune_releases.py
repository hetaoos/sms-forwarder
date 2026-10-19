#!/usr/bin/env python3
"""
同版本收敛：删除与本次构建同属一个 <VERSION> 的历史 Release / Tag。

背景（为什么要删）：版本标识是 <VERSION>-<构建时间>-<短SHA>，每次 push 都能
产出一个新 Release。同一版 VERSION 反复构建时，Release / Tag 会越堆越多，
而设备端 OTA「检查更新」只会用到 tag 列表里最新的那个 —— 旧的那些除了占位
没有任何用处。所以在创建本次 Release 之前，先把同版本的历史清掉，让
一个 VERSION 只留最新一次构建。

匹配规则：本次 tag 的前缀 = <VERSION>-，严格按这个前缀删。用后缀匹配会把
1.0.0 与 11.0.0.1 这种混起来（之前踩过），用整段相等又会连本次构建刚推的
同名 tag 一起删，所以只认前缀。
保留：本次 tag 本身、以及不带时间戳的裸 VERSION tag（如 `1.0.0`）——
后者是手动打的里程碑标签，不属于 CI 产物。

用法（CI 里由 .cnb.yml 的 prune-old-releases 任务调用）：
  python3 script/prune_releases.py "$RELEASE_TAG"           # 真删
  python3 script/prune_releases.py "$RELEASE_TAG" --dry-run  # 只看要删哪些

鉴权：默认读 CNB_TOKEN / CNB_API_ENDPOINT / CNB_REPO_SLUG 三个环境变量
（CI 里都有）；本地可用 --token / --api / --repo 覆盖。
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

DEFAULT_API = "https://api.cnb.cool"

# 删 Release 与删 Tag 是两个接口，别混：
#   Release  : DELETE /-/releases/<release_id>      （id 是数字串，tag 名字不行）
#   Tag      : DELETE /-/git/tags/<tag_name>
# 删 Release 不会连带删 Tag，两样都得做。
PAGE_SIZE = 100


def api_request(url, token, method="GET"):
    req = urllib.request.Request(url, method=method)
    req.add_header("Authorization", "Bearer %s" % token)
    # 平台要求这个 Accept，否则 406
    req.add_header("Accept", "application/vnd.cnb.api+json")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = resp.read()
            return json.loads(body) if body else None
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:200]
        raise SystemExit("[prune_releases] ❌ %s %s -> HTTP %s %s"
                         % (method, url, e.code, detail))


def list_releases(api, repo, token):
    """拉全量 Release（新→旧的顺序由平台保证）。"""
    out = []
    page = 1
    while True:
        data = api_request(
            "%s/%s/-/releases?page=%d&page_size=%d" % (api, repo, page, PAGE_SIZE),
            token)
        if not data:
            break
        out.extend(data)
        if len(data) < PAGE_SIZE:
            break
        page += 1
    return out


def list_tags(api, repo, token):
    out = []
    page = 1
    while True:
        data = api_request(
            "%s/%s/-/git/tags?page=%d&page_size=%d" % (api, repo, page, PAGE_SIZE),
            token)
        if not data:
            break
        out.extend(data)
        if len(data) < PAGE_SIZE:
            break
        page += 1
    return out


def stale(names, keep, prefix):
    """从名字列表里挑出「同版本、但不是本次构建」的那些。

    - 只认严格前缀 <VERSION>-，避免 1.0.0 撞上 11.0.0.1
    - 跳过本次 tag（它是刚刚创建/即将被创建的）
    - 跳过裸 VERSION（`1.0.0` 这种手动里程碑标签）
    """
    doomed = []
    for n in names:
        if n == keep or n == prefix.rstrip("-"):
            continue
        if n.startswith(prefix):
            doomed.append(n)
    return doomed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keep", help="本次构建的 RELEASE_TAG，如 1.0.0-20261004T121442-f2b513a")
    ap.add_argument("--dry-run", action="store_true", help="只列出将要删除的，不真删")
    ap.add_argument("--token", default=os.environ.get("CNB_TOKEN", ""))
    ap.add_argument("--api", default=os.environ.get("CNB_API_ENDPOINT", DEFAULT_API))
    ap.add_argument("--repo", default=os.environ.get("CNB_REPO_SLUG", ""))
    args = ap.parse_args()

    keep = args.keep.strip()
    if not keep:
        raise SystemExit("[prune_releases] ❌ 缺少本次构建的 tag")
    if not args.token or not args.repo:
        raise SystemExit("[prune_releases] ❌ 缺少 CNB_TOKEN / CNB_REPO_SLUG")

    # 本次 tag 形如 <VERSION>-<时间戳>-<短SHA>，取到第一个连字符即为 VERSION。
    # VERSION 里本来就不该有连字符（gen_version.sh 也是这么拼的）。
    prefix = keep.split("-", 1)[0] + "-"
    print("[prune_releases] 本次 tag: %s" % keep)
    print("[prune_releases] 清理范围: 前缀 %s*（裸 %s 保留）"
          % (prefix, prefix.rstrip("-")))

    releases = list_releases(args.api, args.repo, args.token)
    tags = list_tags(args.api, args.repo, args.token)

    doomed_names = set(stale([r["tag_name"] for r in releases], keep, prefix))
    doomed_releases = [(r["tag_name"], r["id"]) for r in releases
                       if r["tag_name"] in doomed_names]
    doomed_tags = stale([t["name"] for t in tags], keep, prefix)

    if not doomed_releases and not doomed_tags:
        print("[prune_releases] ✅ 无同版本历史，跳过")
        return

    print("[prune_releases] 待删 Release %d 个: %s"
          % (len(doomed_releases), [t for t, _ in doomed_releases]))
    print("[prune_releases] 待删 Tag %d 个: %s" % (len(doomed_tags), doomed_tags))
    if args.dry_run:
        print("[prune_releases] 🔍 dry-run，未做任何删除")
        return

    for tag_name, rel_id in doomed_releases:
        api_request("%s/%s/-/releases/%s" % (args.api, args.repo, rel_id),
                    args.token, method="DELETE")
        print("[prune_releases] 🗑️  已删 Release %s (%s)" % (tag_name, rel_id))
    for tag_name in doomed_tags:
        api_request("%s/%s/-/git/tags/%s" % (args.api, args.repo, tag_name),
                    args.token, method="DELETE")
        print("[prune_releases] 🗑️  已删 Tag %s" % tag_name)

    print("[prune_releases] ✅ 清理完成")


if __name__ == "__main__":
    main()
