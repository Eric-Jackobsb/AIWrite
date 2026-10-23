# -*- coding: utf-8 -*-
"""源码克隆检测（重复度审计 · 建议项，**未接入 `check_all`**）

判据：`docs/audit/audit_codebase_20261023.md` §4 的口径 —— 报告**重复率**与被覆盖行数，
供 `ARCH-Z1` 前后对比（目标 ≤ 3 %）。与 `check_all` 的差别：本脚本**只报不判**（退出码恒 0），
因为它衡量的是「结构相似度」而非「硬违规」；接入门禁需先有稳定的棘轮基线策略。

用法：`python source/tools/dup_scan.py [--k 6] [--top 15] [--layers ui,engine]`

口径（与审计报告一致）：
  1. 预处理：去块注释（保留行号）、去行注释、抹掉 raw string、压空白。
  2. 只保留「有意义行」：丢弃纯括号 / 分号 / 预处理 / `namespace` / 访问修饰符等结构性行。
  3. 克隆 = 连续 K（默认 6）个有意义行完全相同 ⇒ 向两侧延伸到最大；同一行只计一次；
     **禁止自重叠**（`j - i < n` 的退化块丢弃，否则「相邻两行相同」会滚成巨型假块）。
  4. 重复率 = 被覆盖的有意义行 / 有意义行总数。

注意事项（实测踩过的坑）：
  * **不要抹掉字符串字面量**：`cli_diag.cpp` 里连续几十行 `std::printf("...")` 一旦抹成
    `std::printf("")` 就全成「重复」—— 一度把重复率虚报到 6.2 %。
  * 注释必须先去掉：否则文件头版权 / 术语表会被判成克隆。
"""

import argparse
import collections
import os
import re
import sys

DEFAULT_LAYERS = ['engine', 'nodes', 'ui', 'app', 'ai', 'web', 'utils']

SKIP_RE = re.compile(
    r'^(#|\{|\}|\);|\)|;|\};|namespace\b|using namespace\b|'
    r'public:|private:|protected:|else\b|break;|return;|'
    r'default:|case\b)')

BLOCK_COMMENT_RE = re.compile(r'/\*.*?\*/', re.S)
LINE_COMMENT_RE = re.compile(r'//[^\n]*')
RAW_STRING_RE = re.compile(r'R"([^(]*)\(.*?\)\1"', re.S)
WS_RE = re.compile(r'\s+')


def repo_root():
    here = os.path.abspath(__file__)                 # <root>/source/tools/dup_scan.py
    return os.path.dirname(os.path.dirname(os.path.dirname(here)))


def clean(text):
    """去注释（保留行号）+ 抹 raw string；**保留**普通字符串字面量。"""
    text = BLOCK_COMMENT_RE.sub(lambda m: '\n' * m.group(0).count('\n'), text)
    text = LINE_COMMENT_RE.sub('', text)
    return RAW_STRING_RE.sub('""', text)


def collect(root, layers):
    tokens = []
    for layer in layers:
        base_dir = os.path.join(root, 'source', layer)
        if not os.path.isdir(base_dir):
            continue
        for dirpath, _dirs, names in os.walk(base_dir):
            for name in sorted(names):
                if not name.endswith(('.cpp', '.h', '.hpp')):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, encoding='utf-8', errors='replace') as fh:
                    body = clean(fh.read())
                rel = os.path.relpath(path, root)
                for lineno, line in enumerate(body.split('\n'), 1):
                    sig = WS_RE.sub(' ', line.strip())
                    if not sig or SKIP_RE.match(sig):
                        continue
                    tokens.append((sig, rel, lineno))
    return tokens


def find_runs(tokens, k):
    total = len(tokens)
    index = collections.defaultdict(list)
    for i in range(total - k + 1):
        index[tuple(t[0] for t in tokens[i:i + k])].append(i)

    seen = set()
    runs = []
    for starts in index.values():
        if len(starts) < 2:
            continue
        for x in range(len(starts)):
            for y in range(x + 1, len(starts)):
                i, j = starts[x], starts[y]
                if (i, j) in seen:
                    continue
                if i > 0 and j > 0 and tokens[i - 1][0] == tokens[j - 1][0]:
                    continue                      # 不是最长块的起点
                n = k
                while (i + n < total and j + n < total
                       and tokens[i + n][0] == tokens[j + n][0]):
                    n += 1
                if j - i < n:
                    continue                      # 自重叠 ⇒ 退化块
                for d in range(n):
                    seen.add((i + d, j + d))
                runs.append((n, i, j))
    return runs


def main(argv=None):
    parser = argparse.ArgumentParser(description='源码克隆检测（重复度审计）')
    parser.add_argument('--k', type=int, default=6, help='窗口行数（默认 6）')
    parser.add_argument('--top', type=int, default=15, help='打印前 N 条（默认 15）')
    parser.add_argument('--layers', default='', help='只扫指定层（逗号分隔，默认全部）')
    args = parser.parse_args(argv)

    layers = ([s.strip() for s in args.layers.split(',') if s.strip()]
              if args.layers else DEFAULT_LAYERS)
    root = repo_root()
    tokens = collect(root, layers)
    total = len(tokens)
    if total < args.k:
        print('有意义行不足，放弃统计')
        return 0

    runs = find_runs(tokens, args.k)
    covered = set()
    for n, i, j in runs:
        for d in range(n):
            covered.add(i + d)
            covered.add(j + d)

    print('layers           : %s' % ','.join(layers))
    print('meaningful lines : %d' % total)
    print('clone runs       : %d' % len(runs))
    print('duplicated lines : %d' % len(covered))
    print('duplication rate : %.1f%%' % (100.0 * len(covered) / total))

    per_layer = collections.Counter()
    layer_total = collections.Counter()
    for idx, tok in enumerate(tokens):
        layer = tok[1].replace('\\', '/').split('/')[1]
        layer_total[layer] += 1
        if idx in covered:
            per_layer[layer] += 1
    print('\n-- 按层 --')
    for layer in layers:
        if not layer_total[layer]:
            continue
        print('%-8s total=%5d  dup=%5d  rate=%5.1f%%'
              % (layer, layer_total[layer], per_layer[layer],
                 100.0 * per_layer[layer] / layer_total[layer]))

    pair = collections.Counter()
    for n, i, j in runs:
        a, b = tokens[i][1], tokens[j][1]
        if a == b:
            continue
        pair[tuple(sorted((a, b)))] += n
    print('\n-- 重复最多的文件对（行数）--')
    for (a, b), n in pair.most_common(args.top):
        print('%5d  %s  <->  %s' % (n, a, b))

    runs.sort(key=lambda r: -r[0])
    print('\n-- 最长克隆块 Top %d --' % min(args.top, len(runs)))
    for n, i, j in runs[:args.top]:
        print('%4d lines  %s:%d   <->   %s:%d'
              % (n, tokens[i][1], tokens[i][2], tokens[j][1], tokens[j][2]))
        print('            | %s' % tokens[i][0][:90])

    self_runs = [r for r in runs if tokens[r[1]][1] == tokens[r[2]][1]]
    if self_runs:
        print('\n-- 同文件内重复块 Top %d --' % min(args.top, len(self_runs)))
        for n, i, j in sorted(self_runs, key=lambda r: -r[0])[:args.top]:
            print('%4d lines  %s:%d  <->  :%d' % (n, tokens[i][1], tokens[i][2],
                                                  tokens[j][2]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
