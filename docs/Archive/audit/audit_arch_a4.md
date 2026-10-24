# audit_arch_a4：ARCH-A4 落地审计报告（依赖单向 CI 断言 · `check_includes.ps1`）

> 批次：`ARCH-A4` · 日期：2026-10-23 · 计划：[refactor/ARCHITECTURE.md](../../refactor/ARCHITECTURE.md) §2.1 迁移路线阶段 3「应用级收口（ARCH-A）」A4
> 性质：**架构约束工具化** —— 把 §1.1 规则 1「依赖单向」从**口头纪律**变成**离线可验证断言**：新增 [../../source/tools/check_includes.ps1](../../source/tools/check_includes.ps1)，并接入 [../../source/tools/check_all.ps1](../../source/tools/check_all.ps1) 作为第 ⑥ 项（`check_all` **19 → 20 项**）。
> 判据：`check_all` **20/20 PASS**（新项 `check_includes（依赖单向）` PASS）· 扫描 **201 文件 / 656 include 行 / 违规 0** · 构建 0/0 · `--run-selftest` PASS · exec 713/0 · graph 151/0 · check_links **1864/0**。

## 1. 背景与目标（承接 §1.1 规则 1 / §4 判据 5）

重构前，「engine/nodes/ai/web/utils 不得反向依赖 ui/app」只写在 ARCHITECTURE.md 里，**没有任何机器判定**。A1~A3 连续拆完应用级服务后，ui 层文件数（68）与 app 层（3）都变过，跨层 include 一旦写错（哪怕只是 IDE 自动补全补了个 `editor_controller.h`），编译**照样通过**（同属一个 exe），错误只会在下一次重构时爆炸。

A4 的目标 = **让违规立刻变红**：

1. 静态扫描层间 include，**只读、零副作用、可离线跑**（与 `check_links.py` 同一条纪律）。
2. 违规 = 失败（退出码 1），并打印 `文件:行 #include "x" ⇒ 归属层`。
3. 接入一键自检，成为每轮收尾的固定一项（不靠人记）。

## 2. 规则定义（脚本 = 唯一执行者）

| 规则 | 内容 | 依据 |
|---|---|---|
| **R1** | `engine/` `nodes/` `ai/` `web/` `utils/` 的 `*.h` / `*.cpp` **禁止** include 归属 `ui/` 或 `app/` 的头文件 | §1.1 规则 1（能力层无 UI 依赖） |
| **R2** | `ui/` 的 `*.h` / `*.cpp` **禁止** include 归属 `app/` 的头文件 | §1.1 唯一合法方向 `app → ui → engine → …` |
| — | `app/` 可依赖所有层；`third_party/` **只作归属表来源**，不参与断言 | 入口层职责 |

**归属判定算法**（关键设计点）：本项目 include **一律写裸文件名**（`#include "graph.h"`，全库带路径的 include 仅 3 处且都在 `third_party/`），所以按 include 文本里的 `ui/` 前缀匹配**一件也抓不到**。脚本因此先建「**裸文件名（小写）→ 所属层**」表，再逐行反查：

1. 扫描 `app/ui/engine/nodes/ai/web/utils/third_party` 下全部 `*.h`，建表；
2. include 目标带路径且首段是层名（`ui/x.h` / `..\ui\x.h`）⇒ 直接按层归属；
3. 否则取文件名末段查表 ⇒ 归属层；
4. 查不到（系统头 / 未扫描的第三方头）⇒ 计入「未归属」，**不判失败**（当前仅 `nlohmann/json.hpp` 1 条）。

**跨层同名头文件 = 归属不明 ⇒ 判失败**（防止「两个 `config.h` 各归一层，脚本静默挑一个」而放行真违规）；当前全库 0 组同名，属预防性条款。

## 3. 脚本输出（判据可读）

```
层              文件   include       违规
--------   ------  --------   ------
app             3        88        0
ui             68       292        0
engine         24        57        0
nodes          10        34        0
ai             44       105        0
web            19        38        0
utils          33        42        0

未归属 include（系统头 / 未扫描的第三方头，不计失败）：1 条
  - web\src\channel_frames.cpp:8  #include "nlohmann/json.hpp"

checked files: 201   include lines: 656   violations: 0
结果: PASS（engine/nodes/ai/web/utils 不含 ui|app 头 · ui 不含 app 头）
```

- 结果行固定含 `checked files:` ⇒ `check_all` 直接抓这一行做摘要（与 `check_links.py` 的 `checked links:` 同款约定）。
- 违规时逐条打印 `文件:行  #include "x"  ⇒ 归属 ui（utils 禁止依赖 ui）` + `结果: FAIL`，退出码 1。

## 4. 反向验证（脚本真的会红）

只证明「现状通过」不够 —— 新断言必须**能失败**，否则等于没接。用临时文件做了三种注入测试（测完即删，未入库）：

| 注入 | 预期 | 实测 |
|---|---|---|
| `utils/include/_tmp_lib_bad.h` include `editor_controller.h`（ui 归属）+ `_tmp_owner.h`（app 归属） | utils 层 2 条违规 | ✅ exit 1 · `utils ... 违规 2` · 明细逐条定位到 `_tmp_lib_bad.h:2 / :3` |
| `ui/include/_tmp_ui_bad.h` include `_tmp_owner.h`（app 归属） | ui 层 1 条违规（R2 生效） | ✅ exit 1 · `ui ... 违规 1` |
| `ui/include/_tmp_dup.h` + `engine/include/_tmp_dup.h`（跨层同名） | 归属不明 ⇒ 失败 | ✅ exit 1 · 打印「头文件跨层同名（归属不明 ⇒ 失败）」 |

三次注入后删除临时文件复跑 ⇒ 回到 `violations: 0` / exit 0（**基线可复原，无残留**）。

## 5. 接入一键自检

- [../../source/tools/check_all.ps1](../../source/tools/check_all.ps1) 新增 **第 ⑥ 项**（在「⑤ 站点台账」与「文档断链」之间，静态检查归堆），原 ⑥ 文档断链 → **⑦**、⑦ 打包 → **⑧**（脚本内注释/打印同步改号，无外部依赖）。
- 判据：退出码 0 ⇒ PASS；摘要行取 `checked files: …`。
- 项数：**19 → 20**（`=== check_all 汇总：共 20 项 · 失败 0 项 ===`）。

## 6. 验证结果

| 验证 | 结果 |
|---|---|
| `check_includes.ps1`（单跑） | ✅ `checked files: 201   include lines: 656   violations: 0` · exit 0 |
| 注入测试 3 种 | ✅ 全部被捕获（见 §4） |
| `tools/check_all.ps1` | ✅ **20/20 全 PASS** · exit 0（新项 PASS） |
| `source/build.ps1` | ✅ 构建 **0 error / 0 warning** |
| `aiwrite.exe --run-selftest` | ✅ PASS（零断言增减） |
| `api_probe.exe --exec-selftest` / `--graph-selftest` | ✅ **713 / 0** · **151 / 0** |
| `check_links.py` | ✅ **1864 / 0** |

## 7. 验收判据对照（§4）

| 判据 | 状态 | 取证 |
|---|---|---|
| **5. 依赖单向** | ✅ **满足** | `check_includes.ps1` 通过：`engine 24 / nodes 10 / ai 44 / web 19 / utils 33` 源文件共 **130 个能力层文件、276 行 include**，归属 `ui` 或 `app` 者 **0**；`ui` 68 文件 292 行 include，归属 `app` 者 **0**（`app` 层当前 0 个公开头 ⇒ R2 为预防性约束） |

## 8. 已知限制

- **只覆盖 `#include "…"` 双引号形式**（项目内部一律双引号；`<…>` 全为系统/第三方头，无跨层意义）。
- **不解析宏拼接 / 条件编译**（`#if` 两侧**都**扫，比编译器更严格 —— 宁可误报不放过）。
- **`source/tools/`（自检程序）不参与断言**：它们是测试端，允许同时引用多层。
- 归属表**按文件名而非路径**（本项目 0 组跨层同名；一旦同名即判失败，届时改名或加路径前缀即可）。

## 9. 回滚点

删除 [../../source/tools/check_includes.ps1](../../source/tools/check_includes.ps1) + `git checkout -- source/tools/check_all.ps1` 即回滚到 A3 形态（`check_all` 回到 19 项；本审计文档可留作登记）。

