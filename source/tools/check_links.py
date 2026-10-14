# -*- coding: utf-8 -*-
"""文档相对链接自检（DBG-L01 收口版 · 纳入 `check_all.ps1` 第 15 项）

判据：**全库 0 断链**（`docs/**/*.md` + `source/README.md` + 仓库根 `*.md`）。
用法：`python source/tools/check_links.py [--quiet]`
退出码：0 = 全部可解析；1 = 有断链（逐条打印 `文件:行  ->  目标`）。

与旧临时脚本（`vcpkg-cache/check_links.py`）的差别（即 `DBG-L01` 的两条残余）：
  ① **不再跨反引号 / 跨行误判**：
     * 先剔除**围栏代码块**（``` / ~~~）与**行内代码跨度**（反引号包裹）里的内容；
     * 链接目标必须**同一行且不含空白**（`](目标)`），跨行写法直接不当作链接；
  ② **可复现 + 可入闸**：仓库根由脚本位置推导（不写死绝对路径）、逐条带行号、退出码供 `check_all.ps1` 判定。
"""

import argparse
import io
import os
import re
import sys

FENCE_RE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE_RE = re.compile(r"`[^`]*`")
LINK_RE = re.compile(r"\]\(([^)\s]+)\)")

SKIP_PREFIXES = ("http://", "https://", "mailto:", "#", "data:")


def repo_root():
    here = os.path.abspath(__file__)                  # <root>/source/tools/check_links.py
    return os.path.dirname(os.path.dirname(os.path.dirname(here)))


def collect_files(root):
    files = []
    docs = os.path.join(root, "docs")
    for base, _dirs, names in os.walk(docs):
        for name in names:
            if name.lower().endswith(".md"):
                files.append(os.path.join(base, name))
    source_readme = os.path.join(root, "source", "README.md")
    if os.path.exists(source_readme):
        files.append(source_readme)
    for name in os.listdir(root):
        if name.lower().endswith(".md"):
            files.append(os.path.join(root, name))
    return sorted(set(files))


def strip_code_spans(line, in_fence):
    """返回（本行可解析文本, 新的 in_fence 状态）"""
    if FENCE_RE.match(line):
        return "", not in_fence
    if in_fence:
        return "", True
    return INLINE_CODE_RE.sub("`code`", line), False


def check(root, quiet=False):
    total = 0
    broken = []
    for path in collect_files(root):
        in_fence = False
        with io.open(path, "r", encoding="utf-8", newline="") as fh:
            for lineno, raw in enumerate(fh, start=1):
                text, in_fence = strip_code_spans(raw, in_fence)
                for match in LINK_RE.finditer(text):
                    target = match.group(1).strip()
                    target = target.strip("<>")
                    if not target or target.startswith(SKIP_PREFIXES):
                        continue
                    target = target.split("#")[0].strip()
                    if not target:
                        continue
                    total += 1
                    resolved = os.path.normpath(
                        os.path.join(os.path.dirname(path), target))
                    if not os.path.exists(resolved):
                        broken.append(("%s:%d" % (os.path.relpath(path, root).replace("\\", "/"),
                                                  lineno), target))
    if not quiet:
        for where, target in broken:
            print("BROKEN  %s  ->  %s" % (where, target))
        print("---")
    print("checked links: %d   broken: %d" % (total, len(broken)))
    return 1 if broken else 0


def main():
    parser = argparse.ArgumentParser(description="文档相对链接自检（全库 0 断链判据）")
    parser.add_argument("--quiet", action="store_true", help="只打印合计行")
    args = parser.parse_args()
    root = repo_root()
    if not os.path.isdir(os.path.join(root, "docs")):
        print("未找到 docs/（仓库根推导失败）: %s" % root)
        return 2
    return check(root, args.quiet)


if __name__ == "__main__":
    sys.exit(main())
