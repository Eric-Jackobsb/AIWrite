# aiwrite 对 imgui-node-editor 的补丁与集成约定

本文件记录 aiwrite 对第三方库 `imgui-node-editor`（v0.9.3）的本地改动、
已知危险与开发约定。**任何后续循环开发涉及本库时，先读本文件。**

## 0. 仓库瘦身记录（2026-10-24）

上游附带的 `examples/`、`external/`（自带 imgui 1.84 WIP、DXSDK、gl3w、ScopeGuard、stb_image）、
`docs/`、`.github/`、`misc/` 已**从本仓库移除** —— 它们从未参与编译（本工程的 CMake 只取
`imgui_node_editor.cpp` / `imgui_node_editor_api.cpp` / `imgui_canvas.cpp` / `crude_json.cpp`）。
删除后仓库减少约 **3.5 MB / 70 个文件**；`external/` 的 ODR 红线仍然有效：
**若将来重新引入上游 `external/`，绝不可加入任何 target 的 include 路径。**

## 1. 配对版本（2026-10-16 核实）

| 组件 | 版本 | 位置 |
|---|---|---|
| vcpkg imgui | **1.90.7-docking**（`IMGUI_VERSION_NUM == 19070`） | `vcpkg-installed/x64-windows/include/imgui.h` |
| 库自带 imgui | **1.84 WIP**（`IMGUI_VERSION_NUM == 18309`） | **已移除（2026-10-24）** · 原 `third_party/imgui-node-editor/external/imgui/`（从未被编译） |
| imgui-node-editor | v0.9.3 | `third_party/imgui-node-editor/`（固定 tag） |

所有 TU（应用 + `imgui_node_editor` 库 + 复现工具）都通过
`imgui::imgui`（vcpkg）提供的 include 路径编译到 **1.90.7**。
`external/imgui`（1.84 WIP）不在任何 include 路径上；该目录已于 2026-10-24 从仓库移除（见 §0）。

## 2. ODR 隐患：同名结构体 + 同签名内联函数 → 链接器静默选错实现

### 危险机制
`external/imgui/imgui.h` 定义了与 vcpkg `imgui.h` **同名但字段布局不同**的结构体
（`ImGuiContext`、`ImVec2`、`ImGuiWindow` 等），且内联函数签名完全相同。
若它被误加入 include 路径：

1. **编译不报错**：每个 TU 各自 `#include "imgui.h"`，预处理器就近解析，各自头文件都自洽；
2. **链接不报错**：跨 TU 的符号名（如 `ImVec2` 的方法、`GetIO()` 等）mangling 相同；
3. **运行期错乱**：库 A 按 1.90.7 布局写的内存，库 B 按 1.84 布局读 → 对象内存错乱，
   崩溃点（断言/访问冲突）**远离**真正的破坏点。

这是典型的「崩溃点 ≠ 破坏点」问题——排障时不要只在崩溃点附近找原因。

### 防护（已落实）
1. **`imgui_node_editor.h` 内的版本锁**：
   ```cpp
   static_assert(IMGUI_VERSION_NUM == 19070, "...");
   ```
   任何包含该头的 TU 若用了别的 imgui.h，会在编译期硬失败并给出中文提示。
   ⚠ 升级 vcpkg imgui 时：先评估 node-editor 兼容性，再同步更新此常量。
2. **CMake 红线注释**（`source/CMakeLists.txt` imgui_node_editor 目标处）：
   禁止把 `third_party/imgui-node-editor/external` 加入任何 target 的 include 路径。
3. **本文件**：留存排查路径与证据。

### 通用约定（防同类问题）
- 第三方目录内自带的 `external/`、`deps/` 等**永不加入 include 路径**；一律用
  vcpkg manifest 里的配对版本。
- 在**第三方边界头**（每个 TU 都包含、且可能同时见到两份同名类型的头）放
  `static_assert` 版本锁，比改 CMake 更可靠（CMake 检查不到头文件被抢占）。
- 遇到「对象内存错乱 → 远端崩溃」时，先怀疑 ODR/ABI 不匹配，再怀疑逻辑错误。

## 3. 本地改动清单

| 文件 | 改动 | 原因 |
|---|---|---|
| `imgui_node_editor.h` | `#include <imgui.h>` 后新增 `IMGUI_VERSION_NUM==19070` 静态断言 | ODR 版本锁（见上） |

除此外无任何源码改动（与上游 v0.9.3 一致）。

## 4. 无头复现工具（ce_repro）

`source/tools/ce_repro.cpp`（临时）：headless 驱动 imgui + node-editor，
复现「新建上级节点 → 编辑器首帧」窗口栈断言。注意：

- **headless 必须手动构建字体图集**，否则 `Render()` 断言 `Font Atlas not built!`
  （这是正常 ImGui 前置条件，不是 bug）：
  ```cpp
  io.Fonts->AddFontDefault();
  unsigned char* px; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  ```
- 验证完目标问题后，应删除 `source/tools/ce_repro.cpp` 与 CMake 中的
  `ce_repro` 目标。

## 5. 相关链接

- 上游：https://github.com/thedmd/imgui-node-editor （v0.9.1 源目录，版本号 0.9.3）
- vcpkg imgui port：`vcpkg-cache/buildtrees/imgui/`

## 6. 上级节点「新建」断言追查结论（2026-10-16 定案）

### 结论
「新建上级节点 → 创建并打开编辑器」的窗口栈断言（Mismatched Begin/BeginChild vs
End/EndChild）**在现网代码里已无法复现**——不是断言修好了，而是「新建」流程自身
**已经坏掉**：`CompositeRegistry::save()` 走严格校验，空白规格（内部图为空、未标记
输出）**永远落盘失败**，所以点「创建」只弹「创建失败：上级节点内部图为空」，
编辑器根本打不开，也就不可能触发编辑器窗口栈断言。用户见到的断言来自更早的
「无文件支撑内存会话」实现（或旧构建）。

### 排查证据（运行时）
1. `--auto-open-composite` 程序化驱动真实对话框自动点「创建」：
   - 修复前：`save(空白)` 被拒 → 日志 `[ERROR] [自动复现] 新建失败: 上级节点内部图为空`。
   - 窗口栈看门狗（`windowstack_crash.txt` + app.log）40 帧栈深恒为 1。
2. 直接走「内存会话」路径 `composite_editor_open(make_id())`（旧实现路径）：
   编辑器首帧 + 后续帧窗口栈全部平衡，进程存活。→ 断言与编辑器绘制本身无关。
3. 编辑器停靠中央节点（与主画布同标签页）的首帧同样平衡。→ 与停靠无关。

### 修复（2026-10-16）
「新建」按设计意图「落盘空白规格 JSON → 打开编辑器读取」实现为**草稿保存**：
- `validate_composite_spec(…, bool allow_incomplete_draft)` / `save(…, bool allow_incomplete_draft)`：
  草稿模式放行「内部图为空 / 未标记输出」两项，其余校验不变；编辑器「保存」仍严格校验。
- 空图跳过 `validateWorkflow`（它会对空图报「工作流为空」）。
- `composite_editor.cpp::create_and_open_new_composite()`：新建落盘用
  `allow_incomplete_draft=true`；打包（`composite_pack_selection`）与编辑器保存不变。

### 保留的调试设施
- `app.cpp` 窗口栈看门狗（常开，栈深≠1 才打印）：以后任何窗口栈失衡都能直接定位
  未配对的窗口（写 `windowstack_crash.txt` + app.log）。
- `--auto-open-composite`：程序化复现新建流程（对话框 → 草稿 → 开编辑器 → 加节点 →
  保存），配合看门狗回归。
- `composite_selftest`（编入 api_probe --exec-selftest）：681 断言全过。


