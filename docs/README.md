# AIwrite 文档中心

> 项目：AI 小说/剧本创作软件（节点工作流 + DeepSeek 后端）
> 当前进度：**M1 ✅ / M2 ✅**；**M3 🟡**（节点编辑器）、**M4 ✅ 核心完成**（文本生成链路）、**M5 🟡 核心切片完成**（图片理解 + 图片显示）；补丁系列 A ✅ / B 进行中（**PB-04 Provider 可插拔化展开计划 [M_patchB.md](actionPlan/M_patchB.md) 待审核**）；M6 未开始
> 最近更新：2026-09-26

---

## 一、文档地图

| 文档 | 内容 | 何时看 |
|---|---|---|
| [ai_writer_nodes.md](ai_writer_nodes.md) | **项目设计文档 v1.0**：需求、架构、数据结构、节点、执行引擎、UI、日志、配置、开发计划、附录 | 了解整体设计 / 写代码前对齐规范 |
| [actionPlan/milestone_plan.md](actionPlan/milestone_plan.md) | 里程碑总体计划（M1–M6）与节奏、总体验收对照 | 看整体排期与当前进度 |
| [actionPlan/](actionPlan/)（M3–M6 + 补丁 A/B） | **进行中 / 待开发**的里程碑与补丁 Action Plan（含 [M_patchB.md](actionPlan/M_patchB.md) Provider 可插拔化 / JSON 配置表草案） | 开发某个里程碑 / 补丁前 |
| [Archive/actionPlan/](Archive/actionPlan/)（M1 · M2 · M_rerun · M_textio） | **已完成**的里程碑 / 补丁计划（归档保留） | 追溯历史决策与验收依据 |
| [Archive/M1_技术验证报告.md](Archive/M1_技术验证报告.md) | M1 实测环境、分层验证结果、V/A 验收对照、问题与解决记录 | 想知道“为什么这么搭环境/踩过哪些坑” |
| [节点编辑器使用说明.md](节点编辑器使用说明.md) | **M2 节点系统 P1/P2** 操作手册（鼠标操作全表、9 节点说明、参数校验、撤销规则、自检命令、20 项人工验证清单、已知限制） | 上手操作节点画布 / 做人工验收 |
| [CHANGELOG.md](CHANGELOG.md) | 全部重要变更（新增/修复/变更/移除）与验证项结果 | 每次看进度、排查“什么时候改的” |
| [../source/README.md](../source/README.md) | 源码结构、构建、运行、调试（VS Code 任务/配置）、常见问题 | 动手构建或运行程序 |

---

## 二、快速开始（人类最短路径）

```powershell
cd F:\GameDao\Tools\AIwrite\source
.\build.ps1                                   # 或：cmake --preset default && cmake --build --preset debug
F:\GameDao\Tools\AIwrite\build\bin\aiwrite.exe
```

- VS Code：`Ctrl+Shift+B` 构建，`F5` 调试（配置见 `.vscode/launch.json`），`Terminal → Run Task` 还有 release / 重装依赖 / 自检 / 看日志等任务。
- Visual Studio 2026：打开 `build\aiwrite.slnx`，选 `aiwrite` 后 F5。
- 验证工具：`api_probe.exe --selftest`（SHA3 + HTTP + 请求构造 + 配置往返）、`webview2_login.exe --selftest`（WebView2 Cookie 链路，退出码 0=PASS）。

---

## 三、当前状态（M1 验收对照）

| 项 | 状态 |
|---|---|
| 构建 / 运行 / 中文界面 / 日志 / 配置（A-01、A-02、A-07、A-08） | ✅ 已验证 |
| HTTP、SHA3（V-04、V-05） | ✅ 已验证 |
| 节点画布渲染与交互（V-02、A-03） | ✅ 已升级为真实节点系统（M2 P1/P2）：节点/连线/参数/撤销全部可鼠标操作，逻辑层自检 70 项全绿 |
| WebView2 登录 + Cookie（V-03 / A-04、A-05） | 🟡 自动化链路已通过（`webview2_login --selftest` → Cookie 5 条）；⏳ 运行 `webview2_login.exe` 手动登录一次 |
| DeepSeek API 生成（V-06 / A-06） | ⏳ 需设置 `DEEPSEEK_API_KEY` |
| M3 节点编辑器验收 | ⏳ 按 [节点编辑器使用说明.md](节点编辑器使用说明.md) §8 的 22 项清单人工确认 |

---

## 四、文档维护约定

1. **任何代码/构建/依赖变更** → 追加到 [CHANGELOG.md](CHANGELOG.md) 的 `[Unreleased]`（完成里程碑时归档为该版本号小节）。
2. **里程碑完成** → 更新 [actionPlan/M*](actionPlan/) 顶部状态行、[milestone_plan.md](actionPlan/milestone_plan.md) 的“状态”列，并产出该里程碑的验证报告（如 `M2_*报告.md`）。
3. **环境、构建方式、目录结构、依赖版本变化** → 同步设计文档 [ai_writer_nodes.md](ai_writer_nodes.md) 的 §2（技术栈/构建）、§18（里程碑状态）、§20（目录结构、配置示例、依赖清单）。
4. **构建/调试流程变化** → 同步 [../source/README.md](../source/README.md) 与 `.vscode/`（launch/tasks）。
5. 报告中记录的“待人工验证项”完成后，在本文件与 CHANGELOG 中把对应行改为 ✅。
