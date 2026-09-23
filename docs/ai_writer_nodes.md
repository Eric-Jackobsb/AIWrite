# AI 小说/剧本创作软件 · 项目设计文档

> 版本：v1.0
> 状态：MVP 设计完成；M1（技术验证 + 项目骨架）已实现
> 最后更新：2026-09-22（同步 M1 实测环境、构建方式与目录结构）
> 技术栈：C++20 / MSVC 14.50（VS 2026）/ CMake 4.4.3 + `Visual Studio 18 2026` 生成器 / vcpkg / Dear ImGui 1.90.7 (docking) / imgui-node-editor 0.9.3 / nlohmann/json
> 后端：DeepSeek 官方 API + 网页版逆向（不使用本地模型）

> 相关文档：[CHANGELOG.md](CHANGELOG.md) ｜ [M1_技术验证报告.md](M1_技术验证报告.md) ｜ [actionPlan/](actionPlan/) ｜ [../source/README.md](../source/README.md)

---

## 目录

1. 项目概述
2. 技术栈与依赖
3. 系统架构
4. 工作流设计
5. 数据结构设计
6. 交互设计
7. 界面布局
8. 推理后端设计
9. 节点具体实现
10. 执行引擎
11. 缓存与输出归档
12. 日志与 Console
13. Output 窗口
14. UI 细节
15. 设置面板
16. 首次运行引导
17. 待办项决议汇总
18. 开发计划
19. 延后与待办
20. 附录

---

## 1. 项目概述

### 1.1 定位

一款基于**节点工作流**的 AI 小说/剧本创作软件。用户通过拖拽节点构建创作流程，导入文字、图片等多模态参考，由 AI 阅读并反向生成文字内容。

### 1.2 核心目标

- 可视化节点工作流编辑
- 支持多模态输入（文本、图片）
- 集成 DeepSeek 官方 API 和网页版
- 工作流可保存、加载、复用

### 1.3 MVP 范围

| 功能 | 是否 MVP |
|---|---|
| 节点工作流编辑 | ✅ |
| 文本输入 | ✅ |
| 图片输入 | ✅ |
| LLM 文本生成 | ✅ |
| VLM 图片理解 | ✅ |
| 文本输出 | ✅ |
| 工作流保存/加载 | ✅ |
| 官方 API + 网页版 | ✅ |
| 视频输入 | ❌ 延后 |
| 音频输入 | ❌ 延后 |
| 本地模型 | ❌ 延后 |
| 子工作流/循环/分支 | ❌ 延后 |
| 节点搜索 | ❌ 延后 |
| 快捷键 | ❌ 延后 |
| 多语言 | ❌ 延后 |
| 浅色主题 | ❌ 延后 |

---

## 2. 技术栈与依赖

### 2.1 核心框架

| 库 | 用途 | 获取方式（M1 实测） |
|---|---|---|
| MSVC 14.50（VS 2026 Insiders） | 编译器 | 系统安装 |
| CMake 4.4.3 | 构建 | 系统安装（生成器 `Visual Studio 18 2026`） |
| vcpkg | 包管理 | 本地实例 `C:\dev\vcpkg`（2024-04 快照，基线 `eb0f108`） |
| Dear ImGui 1.90.7 (docking) | UI | vcpkg（docking-experimental / glfw-binding / opengl3-binding） |
| imgui-node-editor v0.9.3 | 节点画布 | 源码集成 `source/third_party/`（含最小补丁） |
| GLFW 3.4 + OpenGL 3.3 Core | 窗口 / 渲染后端 | vcpkg / 系统 |
| nlohmann/json 3.11.3 | JSON | vcpkg |

### 2.2 功能补充

| 库 | 用途 | 获取方式 |
|---|---|---|
| stb_image | 图片加载 | 单头文件 |
| nativefiledialog-extended | 文件对话框 | vcpkg |
| spdlog | 日志 | vcpkg |
| toml++ | 配置管理 | 单头文件 |
| cpp-httplib | HTTP 客户端 | vcpkg / 单头文件 |
| OpenSSL | SHA3（PoW） | vcpkg |
| WebView2 | 嵌入浏览器 | NuGet / vcpkg |
| glm | 数学库（可选） | vcpkg |
| Font Awesome | 图标（可选） | 源码集成 |

### 2.3 移除

| 库 | 原因 |
|---|---|
| llama.cpp | MVP 不使用本地模型 |
| FFmpeg / avcpp | 视频延后 |
| SQLite | 改用纯文件缓存 |

### 2.4 构建与运行（M1 实测）

| 项目 | 值 |
|---|---|
| 构建目录 | `build/`（唯一；含可用 VS 2026 打开的 `aiwrite.slnx`），产物统一在 `build/bin/` |
| 配置 / 编译 | `cmake --preset default` → `cmake --build --preset debug`（或 `release`）；也可用 `source/build.ps1` |
| 依赖安装 | `source/install-deps.cmd`（vcpkg manifest → `vcpkg-installed/`，下载与二进制缓存 → `vcpkg-cache/`，均在 F: 盘） |
| 依赖安装用 CMake | 固定 `source/../third_party/cmake-3.31.6`（本 vcpkg 快照的 ports 与 CMake 4.x 不兼容） |
| 可执行文件 | `build/bin/aiwrite.exe`（唯一主程序）、`api_probe.exe`（验证工具）、`webview2_login.exe`（登录/取 Cookie） |
| 运行数据 | `C:\Users\<用户>\.brain-ai\`：`outputs/`、`workflows/`、`snapshots/`、`logs/app.log`、`config.toml`、`imgui.ini` |
| 字体 | 运行时加载系统字体（`msyh.ttc` 14px、`consola.ttf` 13px），仓库内不放字体文件 |

---

## 3. 系统架构

### 3.1 分层架构

```
┌─────────────────────────────────────────┐
│ UI 层 (Dear ImGui + imgui-node-editor)  │
├─────────────────────────────────────────┤
│ 工作流引擎层 (Graph / Executor)          │
├─────────────────────────────────────────┤
│ 节点层 (Node / Port / Param / Edge)      │
├─────────────────────────────────────────┤
│ 推理抽象层 (InferenceProvider)           │
├─────────────────────────────────────────┤
│ 后端实现层                                │
│  ├─ DeepSeekOfficialProvider (API)      │
│  └─ DeepSeekWebProvider (网页版逆向)     │
├─────────────────────────────────────────┤
│ 基础设施层 (日志 / 配置 / 文件 / 归档)    │
└─────────────────────────────────────────┘
```

### 3.2 核心模块

| 模块 | 职责 |
|---|---|
| UI | 节点画布、参数面板、菜单栏、工具栏、Console、Output 窗口、设置面板 |
| 工作流引擎 | 图解析、拓扑排序、单线程分帧执行 |
| 节点系统 | 节点注册、端口类型、参数定义 |
| 推理抽象 | 统一 generate / generateStream / generateWithImage |
| 后端实现 | 官方 API、网页版逆向 |
| 基础设施 | 日志、配置、文件对话框、图片加载、输出归档 |

---

## 4. 工作流设计

### 4.1 三层模型

```
Workflow
  └─ Graph
       ├─ Node
       │    ├─ Port
       │    └─ Param
       └─ Edge
```

| 概念 | 说明 |
|---|---|
| Workflow | 完整 `.json` 文件，可保存/加载 |
| Graph | 运行时内存中的有向无环图 |
| Node | 操作单元 |
| Port | 节点输入/输出接口 |
| Edge | 连接两个端口的连线 |
| Param | 节点内部配置参数 |

### 4.2 执行模型

```
用户点击"运行"
 → 解析 Graph
 → 拓扑排序
 → 单线程分帧执行
 → 每个节点：读输入 → 执行 → 写输出
 → 输出归档到文件
```

### 4.3 节点分类

| 分类 | 用途 | 节点 |
|---|---|---|
| 输入节点 | 提供数据 | Text Input / Image Input |
| 处理节点 | 转换数据 | Prompt Template / Text Merge |
| 配置节点 | 配置提供商 | Provider Config |
| 推理节点 | 调用 AI | LLM Generate / VLM Generate |
| 输出节点 | 展示结果 | Text Output / Image Preview |

### 4.4 MVP 节点清单（9 个）

| 编号 | 节点 | 分类 | 输入 | 输出 |
|---|---|---|---|---|
| N-01 | Text Input | 输入 | — | text |
| N-02 | Image Input | 输入 | — | image |
| N-03 | Prompt Template | 处理 | vars | text |
| N-04 | Text Merge | 处理 | texts[] | text |
| N-05 | Provider Config | 配置 | — | provider |
| N-06 | LLM Generate | 推理 | prompt, provider | text |
| N-07 | VLM Generate | 推理 | prompt, image, provider | text |
| N-08 | Text Output | 输出 | text | — |
| N-09 | Image Preview | 输出 | image | — |

### 4.5 端口类型系统

| 类型 | 说明 | 颜色 |
|---|---|---|
| text | 字符串 | 白 `#E0E0E0` |
| image | 图片路径 | 绿 `#64C864` |
| provider | AI 提供商句柄 | 蓝 `#6496FF` |
| number | 数值 | 灰 `#B4B4B4` |
| any | 任意类型 | 紫 `#C896FF` |

类型兼容规则：

- 同类型可连
- 任意类型可连到 any
- any 可连到任意类型（运行时检查）
- 其他不允许

### 4.6 节点状态

| 状态 | 说明 | 颜色 |
|---|---|---|
| idle | 未执行 | 灰 `#666666` |
| waiting | 等待依赖 | 黄 `#D4B830` |
| running | 执行中 | 蓝 `#4A90E2` |
| done | 完成 | 绿 `#50B050` |
| error | 失败 | 红 `#E25050` |
| skipped | 跳过 | 暗灰 `#4A4A4A` |

### 4.7 工作流 JSON Schema

```json
{
  "version": "1.0",
  "name": "文本续写",
  "description": "输入开头，AI续写",
  "nodes": [
    {
      "id": "n1",
      "type": "TextInput",
      "title": "输入",
      "position": { "x": 100, "y": 200 },
      "params": { "text": "从前有座山" }
    }
  ],
  "edges": [
    {
      "id": "e1",
      "from": { "node": "n1", "port": "text" },
      "to": { "node": "n3", "port": "prompt" }
    }
  ],
  "viewport": { "x": 0, "y": 0, "zoom": 1.0 }
}
```

---

## 5. 数据结构设计

### 5.1 Node

```cpp
struct Node {
    std::string id;
    std::string type;
    std::string title;
    ImVec2 position;
    ImVec2 size;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::vector<Param> params;
    NodeState state;
    std::string error_message;
    nlohmann::json output_cache;
};
```

### 5.2 Port

```cpp
enum class PortType { Text, Image, Provider, Number, Any };
enum class PortDirection { Input, Output };

struct Port {
    std::string id;
    std::string name;
    std::string display_name;
    PortType type;
    PortDirection direction;
    bool is_optional;
    bool is_variadic;
    nlohmann::json default_value;
    std::string description;
};
```

### 5.3 Param

```cpp
enum class ParamType { String, Text, Int, Float, Bool, Enum, File, Directory, Color };

struct Param {
    std::string id;
    std::string name;
    std::string display_name;
    ParamType type;
    nlohmann::json value;
    nlohmann::json default_value;
    std::optional<double> min_value;
    std::optional<double> max_value;
    std::optional<double> step;
    std::vector<std::string> enum_options;
    std::string description;
};
```

### 5.4 Edge

```cpp
struct Edge {
    std::string id;
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
    bool is_valid;
    std::string error_message;
};
```

### 5.5 Graph

```cpp
class Graph {
public:
    std::string id;
    std::string name;
    std::vector<Node> nodes;
    std::vector<Edge> edges;

    Node* findNode(const std::string& id);
    Edge* findEdge(const std::string& id);
    void addNode(const Node& node);
    void removeNode(const std::string& id);
    void addEdge(const Edge& edge);
    void removeEdge(const std::string& id);
    bool validate(std::string& error);
    bool hasCycle();
    std::vector<std::string> topologicalSort();
};
```

### 5.6 Workflow

```cpp
struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float zoom = 1.0f;
};

struct Workflow {
    std::string version = "1.0";
    std::string name;
    std::string description;
    Graph graph;
    Viewport viewport;
    nlohmann::json toJson() const;
    static Workflow fromJson(const nlohmann::json& j);
};
```

### 5.7 ExecutionContext

```cpp
class ExecutionContext {
public:
    Graph* graph;
    NodeRegistry* registry;
    Logger* logger;
    OutputWindow* output_window;

    std::unordered_map<std::string, NodeState> node_states;
    std::unordered_map<std::string, nlohmann::json> node_outputs;
    std::unordered_map<std::string, std::string> node_errors;

    bool cancelled = false;
};
```

### 5.8 快照撤销栈

```cpp
class UndoStack {
public:
    void push(const Workflow& snapshot);
    bool canUndo() const;
    bool canRedo() const;
    std::optional<Workflow> undo();
    std::optional<Workflow> redo();
    void clear();
private:
    std::vector<Workflow> undo_stack_;
    std::vector<Workflow> redo_stack_;
    size_t max_size_ = 50;
};
```

---

## 6. 交互设计

### 6.1 节点编辑

| 操作 | 交互 |
|---|---|
| 添加节点 | 右键画布 → 节点菜单；拖拽节点库 |
| 选择节点 | 单击；`Ctrl+单击` 多选；框选 |
| 移动节点 | 拖拽节点标题 |
| 删除节点 | 右键节点 → 删除 |
| 复制粘贴 | 右键节点 → 复制；右键画布 → 粘贴 |
| 重命名 | 双击节点标题 |

### 6.2 端口连接

| 操作 | 交互 |
|---|---|
| 创建连接 | 从输出端口拖拽到输入端口 |
| 取消连接 | 拖到空白处 |
| 删除连接 | 单击选中 + 右键删除 |
| 连接验证 | 类型不兼容变红；输入已连接提示替换 |

### 6.3 参数编辑

- 节点选中后右侧显示参数面板
- 参数控件：String / Text / Int / Float / Bool / Enum / File / Directory / Color
- 参数验证：超出范围、必填为空、格式错误、文件不存在

### 6.4 画布操作

| 操作 | 交互 |
|---|---|
| 平移 | 中键拖拽 |
| 缩放 | 滚轮（以鼠标为中心），范围 0.1x ~ 4.0x |
| 网格 | 可选显示，**使用 imgui-node-editor 内置网格（32px 固定）** |

> **实现说明（M2 更新）**：原「20px 自绘网格」作废——自绘网格需要每帧逐像素构建 draw list，与
> imgui-node-editor 的网格互相叠加，且缩放时产生混叠。现改为只用编辑器内置网格，仅由
> `config.toml` 的 `ui.show_grid` 控制显隐（`ed::StyleColor_Grid` 透明度）。
> 缩放范围通过 `ed::Config::CustomZoomLevels`（0.1/0.25/0.5/0.75/1/1.5/2/3/4）实现，与本节一致。

### 6.5 执行控制

| 操作 | 交互 |
|---|---|
| 运行工作流 | 工具栏"运行"按钮 |
| 停止执行 | 工具栏"停止"按钮 |
| 进度显示 | 状态栏显示节点进度 |
| 节点反馈 | Running 流动边框；Done 绿勾；Error 红叉；Skipped 变暗 |

### 6.6 撤销/重做

- 快照方案
- 栈深度 50
- 工具栏按钮
- 加载工作流清空栈

> **实现说明（M2）**：`engine::UndoStack` 保存整张 `Graph` 快照，深度上限 50（超出丢弃最旧）。
> **一次完整操作 = 一次快照**：创建节点、删除节点/连线、复制粘贴、重命名提交、连线新建/替换/删除、
> **拖拽移动结束**、**参数编辑结束**（`ImGui::IsItemDeactivatedAfterEdit`；控件被激活时
> `IsItemActivated` 先压快照，保证快照是修改前的状态）。产生新操作会丢弃重做分支；「新建/清空」
> 与后续「加载工作流」都会清空撤销栈。撤销/重做后清空画布选择，避免悬空引用。

### 6.7 快捷键

**MVP 不保留任何快捷键，只保留鼠标操作。**

> **实现说明（M2）**：已显式调用 `ed::EnableShortcuts(false)` 关闭 imgui-node-editor 的内置快捷键
> （含 Delete 删除、Ctrl+A 全选等），全部功能改为鼠标可达：右键画布分类新建、右键节点复制/删除/改名、
> 右键连线删除、工具栏「撤销/重做/复制/粘贴/删除选中/新建/示例」、菜单栏「文件/编辑/视图」。
> 画布上的 Ctrl+单击多选与左键框选属于**鼠标手势**，予以保留。

---

## 7. 界面布局

### 7.1 默认布局（节点库和参数面板隐藏）

```
┌─────────────────────────────────────────────────────────────┐
│ 菜单栏: 文件 编辑 视图 运行 帮助                             │
│ 工具栏: [新建][打开][保存] [撤销][重做] [运行][停止]          │
│ 视图切换: [节点库] [参数面板] [Console] [Output]              │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│                         画布                                 │
│                    ┌────┐   ┌────┐                           │
│                    │节点│───│节点│                           │
│                    └────┘   └────┘                           │
│                                                              │
├──────────────────────────────────┬──────────────────────────┤
│ Console                          │ 状态栏                    │
│ [10:23:01] INFO  节点开始         │ 运行中 | 3/10 | 00:05    │
│ [10:23:05] ERROR 调用失败         │ 当前: LLM Generate        │
│ [清空] [过滤] [复制] [导出]        │                          │
└──────────────────────────────────┴──────────────────────────┘
```

### 7.2 区域说明

| 区域 | 默认 | 可切换 |
|---|---|---|
| 菜单栏 | 显示 | — |
| 工具栏 | 显示 | — |
| 视图切换 | 显示 | — |
| 节点库 | **隐藏** | 可切换 |
| 画布 | 显示 | — |
| 参数面板 | **隐藏** | 可切换 |
| Console | 显示 | 可切换 |
| 状态栏 | 显示 | — |
| Output 窗口 | 隐藏 | 可切换 |
| 设置面板 | 隐藏 | 可切换 |

---

## 8. 推理后端设计

### 8.1 统一抽象层

```cpp
class InferenceProvider {
public:
    virtual ~InferenceProvider() = default;
    virtual std::string generate(const std::string& prompt, const GenerateParams& params) = 0;
    virtual void generateStream(const std::string& prompt, const GenerateParams& params,
                                std::function<void(const std::string&)> callback) = 0;
    virtual std::string generateWithImage(const std::string& prompt, const std::string& image_path,
                                          const GenerateParams& params) = 0;
    virtual std::string name() const = 0;
    virtual bool supportsVision() const = 0;
};
```

### 8.2 后端类型

| 后端 | 说明 | 状态 |
|---|---|---|
| DeepSeekOfficialProvider | 官方 API，兼容 OpenAI 格式 | MVP |
| DeepSeekWebProvider | 网页版逆向，零成本 | MVP |

**无 AutoProvider（T-07 已取消 auto）。**

### 8.3 后端选择

用户在 Provider Config 节点中手动指定：

| 模式 | 说明 |
|---|---|
| official | API Key |
| web | 网页版登录 |

### 8.4 API 密钥安全

- 使用 Windows Credential Manager 存储
- 配置文件只存引用，不存明文
- 内存中使用后清除
- 日志和归档不包含 Key

### 8.5 网页版逆向要点

- WebView2 有头登录，用户手动操作
- Cookie 从 WebView2 提取，存内存，程序退出即销毁
- PoW 用 C++ 重写（SHA3）
- SSE 流式解析独立模块
- 会话失效时提示重新登录，不自动刷新

---

## 9. 节点具体实现

### 9.1 通用 executor 签名

```cpp
nlohmann::json execute(
    const nlohmann::json& inputs,
    const nlohmann::json& params,
    ExecutionContext& ctx
);
```

### 9.2 各节点实现

| 节点 | 核心逻辑 |
|---|---|
| Text Input | 输出参数中的文本 |
| Image Input | 输出图片路径，校验文件存在 |
| Prompt Template | 将 `{var}` 替换为输入值 |
| Text Merge | 合并多个文本输入，支持分隔符和顺序 |
| Provider Config | 输出 provider 句柄，校验配置完整性 |
| LLM Generate | 读取 prompt + provider，调用后端，流式回调 |
| VLM Generate | 同 LLM，但多了图片输入 |
| Text Output | 推送文本到 Output 窗口，透传 |
| Image Preview | 推送图片到 Output 窗口，透传 |

### 9.3 错误处理

所有节点抛出 `NodeError`，由执行引擎捕获并标记节点状态为 error。

---

## 10. 执行引擎

### 10.1 单线程分帧执行

| 项目 | 决议 |
|---|---|
| 执行模型 | 单线程 + 分帧 |
| 调度 | 主线程每帧推进一个节点状态 |
| HTTP 调用 | 独立线程（1 个） |
| 总线程数 | 2 个（主线程 + HTTP 线程） |

### 10.2 节点执行状态机

```
Collecting → CheckingCache → Running → Waiting → Finishing → Done
                                                 ↓
                                               Error
```

### 10.3 取消机制

```cpp
bool cancelled = false;

void Executor::cancel() {
    cancelled = true;
    // 通知 HTTP 线程中断
}
```

### 10.4 失败处理

- 节点失败 → 标记 error
- 下游节点 → 标记 skipped
- 无关分支 → 继续执行

---

## 11. 缓存与输出归档

### 11.1 目录结构

```
~/.brain-ai/
├── outputs/
│   ├── 2026-09-21/
│   │   ├── 103045_llm_generate_a3f2.txt
│   │   ├── 103102_vlm_generate_b7c1.txt
│   │   └── 103130_text_output.txt
│   └── 2026-09-22/
│       └── ...
├── workflows/
│   ├── text-continue.json
│   └── ...
├── snapshots/
│   └── (撤销快照)
├── logs/
│   ├── app.log
│   └── workflow.log
├── config.toml
└── recent.json
```

### 11.2 Output 归档

| 项目 | 决议 |
|---|---|
| 命名 | `HHMMSS_type_hash.txt` |
| 内容 | meta + 正文 |
| 图片 | 只存路径，不复制 |
| 保留 | 30 天 |
| 清理 | 程序启动 + 每天一次 |

### 11.3 Workflow 存储

| 项目 | 决议 |
|---|---|
| 位置 | `~/.brain-ai/workflows/` |
| 格式 | JSON |
| 清理 | 不自动清理 |

### 11.4 不做缓存命中

**放弃缓存复用，只做输出归档。**

理由：

- 工作流执行时多是新输入
- 缓存命中收益有限
- 用户主要看历史输出

---

## 12. 日志与 Console

### 12.1 日志级别

| 级别 | 颜色 | 用途 |
|---|---|---|
| INFO | 灰白 `#E0E0E0` | 正常流程 |
| WARN | 黄 `#FFCC33` | 可恢复问题 |
| ERROR | 红 `#FF4D4D` | 失败 |

### 12.2 输出位置

| 位置 | 内容 |
|---|---|
| Console | 全部，可过滤 |
| `app.log` | 应用日志 |
| `workflow.log` | 工作流执行日志 |

### 12.3 日志格式

```
[2026-09-21 10:30:45.123] [INFO] 工作流开始: 文本续写 (4 节点)
[2026-09-21 10:30:45.124] [INFO] [TextInput] 节点完成 (1ms)
[2026-09-21 10:30:52.456] [INFO] [LLMGenerate] 节点完成 (7330ms)
[2026-09-21 10:30:52.460] [INFO] 工作流完成 (总耗时 7337ms, 4/4 成功)
```

### 12.4 Console 实现

| 项目 | 决议 |
|---|---|
| 存储 | 环形缓冲，最多 10000 条 |
| 渲染 | ImGuiListClipper 虚拟化 |
| 过滤 | 级别 + 搜索 |
| 自动滚动 | 用户滚动检测 |
| 复制 / 导出 | 过滤后的日志 |
| 清空 | 内存日志 |
| 折叠 | 可折叠为一行 |

### 12.5 日志保留

| 项目 | 值 |
|---|---|
| 单文件上限 | 10 MB |
| 滚动文件数 | 5 |
| 总占用上限 | 50 MB |
| 保留天数 | 30 天 |

---

## 13. Output 窗口

### 13.1 窗口类型

**停靠窗口**（不做独立窗口）。

### 13.2 标签页

| 项目 | 决议 |
|---|---|
| 每个输出节点一个标签 | 确认 |
| 标签去重，同节点覆盖 | 确认 |
| 标题 = 节点标题 | 确认 |
| 双击节点打开标签 | 确认 |
| 右键菜单"在 Output 窗口打开" | 确认 |

### 13.3 内容渲染

| 类型 | 渲染 |
|---|---|
| 文本 | 自动换行 + 可滚动 |
| 图片 | 适应窗口 + 保持宽高比 |
| 图片纹理 | 缓存 |

### 13.4 历史记录

| 项目 | 决议 |
|---|---|
| 默认 | 关闭 |
| 开启后 | 保留最近 N 次 |
| 最大历史 | 10 次 |

### 13.5 操作

| 按钮 | 功能 |
|---|---|
| 复制 | 复制到剪贴板 |
| 导出 | 导出为文件 |
| 打开文件夹 | 打开归档文件夹 |

### 13.6 状态持久化

| 项目 | 持久化 |
|---|---|
| 窗口位置 / 大小 | ✅ |
| 是否打开 | ✅ |
| 停靠状态 | ✅ |
| 标签 | ❌ |

---

## 14. UI 细节

### 14.1 节点结构

```
┌─────────────────────────────┐
│ ● LLM Generate          ✓   │  ← 标题栏
├─────────────────────────────┤
│ ● prompt          text ●    │  ← 输入端口
│ ● provider     provider ●   │
├─────────────────────────────┤
│ 温度: 0.7                    │  ← 参数预览
│ 长度: 2048                   │
├─────────────────────────────┤
│ 耗时: 4.2s                   │  ← 状态信息
│ 512 tokens                   │
└─────────────────────────────┘
```

### 14.2 节点尺寸

| 属性 | 值 |
|---|---|
| 最小宽度 | 200px |
| 最大宽度 | 400px |
| 标题栏高度 | 28px |
| 端口行高度 | 22px |
| 内边距 | 8px |
| 圆角 | 4px |

### 14.3 分类配色

| 分类 | 颜色 | 十六进制 |
|---|---|---|
| 输入 | 青绿 | `#4A9E8F` |
| 处理 | 蓝灰 | `#5A7A9E` |
| 配置 | 紫 | `#7A5A9E` |
| 推理 | 深蓝 | `#3A6EA5` |
| 输出 | 橙 | `#C47A3A` |

### 14.4 界面背景

| 元素 | 颜色 |
|---|---|
| 画布背景 | `#1E1E1E` |
| 网格线 | `#2A2A2A` |
| 节点背景 | `#2D2D2D` |
| 节点边框 | `#3A3A3A` |
| 选中边框 | `#4A90E2` |
| 面板背景 | `#252525` |
| Console 背景 | `#1A1A1A` |

### 14.5 字体

| 用途 | 字体 | 大小 |
|---|---|---|
| UI 主字体 | 微软雅黑 | 14px |
| 参数文字 | 微软雅黑 | 13px |
| Console 日志 | Consolas | 13px |
| Output 文本 | Consolas | 13px |
| 图标 | Font Awesome | 14px |

### 14.6 连接线

| 状态 | 样式 |
|---|---|
| 正常 | 贝塞尔曲线，2px |
| 选中 | 贝塞尔曲线，3px，高亮 |
| 无效 | 红色虚线，2px |
| 拖拽中 | 半透明，2px |

### 14.7 端口

| 方向 | 形状 |
|---|---|
| 输入 | 左侧圆形 |
| 输出 | 右侧圆形 |

| 属性 | 值 |
|---|---|
| 半径 | 5px |
| 悬停放大 | 7px |

### 14.8 主题

**深色主题**（唯一 MVP 主题）。

---

## 15. 设置面板

### 15.1 形式

**可停靠独立窗口**。

### 15.2 六分类

| 分类 | 内容 |
|---|---|
| 通用 | 语言、启动行为 |
| 界面 | 面板显示、Console、节点样式 |
| 输出 | 归档路径、保留天数、历史记录 |
| 提供商 | API Key、网页版登录 |
| 网络 | 超时、重试 |
| 高级 | 配置文件、日志、重置、导出/导入 |

### 15.3 保存与生效

| 设置类型 | 保存时机 | 生效时机 |
|---|---|---|
| 开关 / 下拉 | 立即 | 立即 |
| 输入框 | 失焦 | 立即 |
| 滑块 | 释放 | 立即 |
| API Key | 点击"更新" | 下次调用 |

### 15.4 重置

| 操作 | 说明 |
|---|---|
| 重置本类 | 每分类底部 |
| 重置全部 | 高级设置，二次确认 |

规则：

- 重置不删除 API Key
- 重置不删除输出归档

### 15.5 导出 / 导入

- 导出配置为 `.toml`
- 从 `.toml` 导入

---

## 16. 首次运行引导

### 16.1 检测

配置文件不存在 → 首次运行。

### 16.2 三步引导

| 步骤 | 内容 |
|---|---|
| 1 | 欢迎介绍 |
| 2 | 选择示例（6 个 + 空白） |
| 3 | 配置提供商 |

### 16.3 跳过

每步可跳过。跳过提供商时提示一次。

### 16.4 完成后

- 保存配置文件
- 加载选中示例
- 进入主界面

### 16.5 重新显示

设置 → 高级 → `[重新显示引导]`。

---

## 17. 待办项决议汇总

### 17.1 第一批（T-01 ~ T-06）

| 编号 | 内容 | 状态 |
|---|---|---|
| T-01 | 网页版逆向：WebView2 + C++ PoW + SSE | 确认 |
| T-02 | 远程配置管理，Provider Config 节点 | 确认 |
| T-03 | 纯文件缓存（outputs + workflows） | 确认 |
| T-04 | 多语言界面 | 延后 |
| T-05 | 主题 | 延后 |
| T-06 | 绿色版打包 | 确认 |

### 17.2 第二批（T-07 ~ T-20）

| 编号 | 内容 | 状态 |
|---|---|---|
| T-07 | 取消 auto，用户手动指定后端 | 确认 |
| T-08 | API Key 存 Windows Credential Manager | 确认 |
| T-09 | Console 面板（INFO / WARN / ERROR） | 确认 |
| T-10 | 节点视觉反馈 + 工作流进度 + 流式输出 | 确认 |
| T-11 | 混合模式 + 独立 Output 窗口 | 确认 |
| T-12 | 移除 Sampler Config，采样参数放推理节点 | 确认 |
| T-13 | 6 个示例工作流 | 确认 |
| T-14 | 快照方案撤销/重做 | 确认 |
| T-15 | 语义化版本 MAJOR.MINOR | 确认 |
| T-16 | 配置文件含 config_version | 确认 |
| T-17 | 加载/运行前/编辑时三级校验 | 确认 |
| T-18 | 每节点一个注册函数 | 确认 |
| T-19 | spdlog 三级别，Console + 文件双输出 | 确认 |
| T-20 | 不成立（无隐式上下文） | 关闭 |

### 17.3 第三批（C-01 ~ C-10）

| 编号 | 内容 | 状态 |
|---|---|---|
| C-01 | 节点具体实现 | 确认 |
| C-02 | 单线程分帧执行 + HTTP 线程 | 确认 |
| C-03 | 纯文件归档（outputs + workflows） | 确认 |
| C-04 | Console 实现（环形缓冲 + 虚拟化） | 确认 |
| C-05 | Output 停靠窗口 | 确认 |
| C-06 | UI 细节（节点样式 / 配色 / 字体） | 确认 |
| C-07 | 首次运行引导（三步） | 确认 |
| C-08 | 设置面板（六分类） | 确认 |
| C-09 | 错误提示 UI | 已覆盖（T-09） |
| C-10 | 工作流执行日志 | 确认 |

---

## 18. 开发计划

### 18.1 阶段划分

| 阶段 | 目标 | 全职 | 业余 |
|---|---|---|---|
| Phase 0 | 技术验证 + 项目骨架 | 3–5 天 | 1–2 周 |
| Phase 1 | 核心工作流引擎 | 1–2 周 | 3–4 周 |
| Phase 2 | 节点编辑器 UI | 1–2 周 | 3–4 周 |
| Phase 3 | 推理后端集成 | 1–2 周 | 3–4 周 |
| Phase 4 | 多模态集成 | 1–2 周 | 3–4 周 |
| Phase 5 | 工作流功能完善 | 1–2 周 | 3–4 周 |
| Phase 6 | 优化 + 打包 | 1–2 周 | 3–4 周 |
| **合计** | **MVP** | **约 2.5 个月** | **约 5–7 个月** |

### 18.2 Phase 0 任务

| 编号 | 任务 |
|---|---|
| P0-01 | 环境搭建（CMake + vcpkg + MSVC） |
| P0-02 | Dear ImGui 集成 |
| P0-03 | imgui-node-editor 集成 |
| P0-04 | WebView2 集成验证 |
| P0-05 | cpp-httplib + OpenSSL 集成 |
| P0-06 | DeepSeek API 最小调用 |
| P0-07 | 项目骨架 |

### 18.3 里程碑

| 里程碑 | 版本 | 标志 | 状态 |
|---|---|---|---|
| M1：技术验证 | v0.1 | ImGui + 节点编辑器 + API 跑通 | ✅ ImGui 与节点编辑器已跑通；API 待 Key（见 [M1_技术验证报告.md](M1_技术验证报告.md)） |
| M2：引擎跑通 | v0.2 | 能加载 JSON 工作流并执行 | ⏳ 未开始 |
| M3：可视化编辑 | v0.3 | 能拖拽编辑工作流 | ⏳ 未开始 |
| M4：文本生成 | v0.4 | 能生成文本 | ⏳ 未开始 |
| M5：多模态 | v0.5 | 能读图生成文字 | ⏳ 未开始 |
| M6：MVP | v1.0 | 功能完整，可自用 | ⏳ 未开始 |

> 变更明细见 [CHANGELOG.md](CHANGELOG.md)。

---

## 19. 延后与待办

### 19.1 延后功能

| 功能 | 状态 |
|---|---|
| 视频输入 | 延后 |
| 音频输入 | 延后 |
| 本地模型 | 延后 |
| 子工作流 | 延后 |
| 循环节点 | 延后 |
| 条件分支 | 延后 |
| 节点搜索 | 延后 |
| 快捷键 | 延后 |
| 小地图 | 延后 |
| 自动吸附 | 延后 |
| 节点折叠/展开 | 延后 |
| 对齐辅助线 | 延后 |
| 多语言界面 | 延后 |
| 浅色主题 | 延后 |

### 19.2 待讨论

| 编号 | 内容 |
|---|---|
| — | 无（MVP 设计已完成） |

---

## 20. 附录

### 20.1 目录结构（M1 已落地）

```
AIwrite/                              （= F:\GameDao\Tools\AIwrite）
├── .vscode/                          构建/调试配置（launch.json / tasks.json / settings.json）
├── build/                            唯一构建目录（VS 2026 生成器；含 aiwrite.slnx）
│   └── bin/                          aiwrite.exe / api_probe.exe / webview2_login.exe
├── docs/                             设计文档、Action Plan、CHANGELOG、验证报告
├── third_party/cmake-3.31.6/         仅用于 vcpkg 依赖安装的 CMake
├── vcpkg-cache/                      vcpkg 下载 / 二进制缓存 / buildtrees
├── vcpkg-installed/                  vcpkg 依赖安装树（x64-windows/include、lib、bin）
└── source/                           工程根（CMakeLists.txt、vcpkg.json 所在）
    ├── src/
    │   ├── main.cpp                  入口（--console 可选控制台）
    │   ├── ui/
    │   │   ├── app.cpp               ✅ 窗口/主题/字体/停靠/菜单/工具栏/状态栏
    │   │   ├── editor_state.cpp      ✅ 编辑器状态（Graph + UndoStack + 复制粘贴 + 示例工作流）
    │   │   ├── node_canvas.cpp       ✅ 节点画布（渲染 + 全部鼠标交互 + 右键菜单）
    │   │   ├── node_library.cpp      ✅ 节点库（9 节点按分类，点击添加）
    │   │   ├── toolbar.cpp           ✅ 工具栏（撤销/重做/复制/粘贴/删除/新建/示例）
    │   │   ├── property_panel.cpp    ✅ 参数面板（9 种参数控件 + 校验提示）
    │   │   ├── theme.h               ✅ 分类/端口/状态配色（设计 14.3 / 4.5 / 4.6）
    │   │   ├── console_panel.cpp     ✅ Console 面板（12.4）
    │   │   ├── output_window.cpp     (M5) Output 停靠窗口
    │   │   ├── settings_panel.cpp    (M6) 设置面板
    │   │   └── welcome_window.cpp    (M6) 首次运行引导
    │   ├── engine/                   ✅ 已落地（M2 P1）
    │   │   ├── graph.cpp             数据模型：Node/Port/Param/Edge/Graph + 校验 + 增删
    │   │   ├── node_registry.cpp     9 个节点的注册表（类型/端口/参数/默认值）
    │   │   ├── undo_stack.cpp        快照式撤销/重做（深度 50）
    │   │   └── (M4) executor.cpp     工作流执行器
    │   ├── nodes/                    (M4/M5) 9 个节点实现 + register.cpp
    │   ├── ai/                       (M4) inference_provider / deepseek_*_provider
    │   └── utils/
    │       ├── paths.cpp             ✅ ~/.brain-ai 数据目录
    │       ├── log.cpp               ✅ spdlog 双 sink + 环形缓冲
    │       ├── config.cpp            ✅ toml++ 读写 config.toml
    │       ├── crypto.cpp            ✅ SHA3-256（M4 PoW 复用）
    │       └── file_dialog.cpp       ✅ 原生文件/目录对话框（nativefiledialog-extended）
    ├── ports/nativefiledialog-extended/  ✅ overlay port（vcpkg 快照缺此包）
    ├── vcpkg-configuration.json      ✅ overlay-ports: ["./ports"]
    ├── tools/                        ✅ 验证 / 自检工具
    │   ├── api_probe.cpp             V-04 / V-05 / V-06 + `--graph-selftest`（图模型自检）
    │   └── webview2_login.cpp        V-03
    ├── third_party/imgui-node-editor/ ✅ v0.9.3 源码集成（含最小补丁）
    ├── cmake/                        ✅ 构建脚本片段（运行时 DLL 同步）
    ├── assets/{fonts,icons}/         可选覆盖资源（默认用系统字体）
    ├── workflows/examples/           (M6) 6 个示例工作流
    ├── CMakeLists.txt / vcpkg.json / CMakePresets.json
    ├── build.ps1 / dev.ps1 / install-deps.cmd
    └── README.md
```

### 20.2 配置示例

> ✅ 已实现：M1 首次运行会自动在 `~/.brain-ai/config.toml` 生成下列内容（`utils/config.cpp`，字段与本节完全一致）。

```toml
config_version = 1

[general]
language = "zh-CN"
startup = "welcome"

[ui]
show_node_library = false
show_property_panel = false
show_console = true
show_output_window = false
console_height = 120
show_grid = true
grid_size = 20
running_animation = true

[output]
archive_dir = "~/.brain-ai/outputs"
ttl_days = 30
auto_open_on_complete = false
keep_history = false
max_history = 10

[timeout]
connect_ms = 10000
first_byte_ms = 30000
stream_idle_ms = 300000

[error]
retry_enabled = true
retry_count = 1
retry_interval_ms = 2000

[advanced]
config_path = "~/.brain-ai/config.toml"
log_dir = "~/.brain-ai/logs"
log_ttl_days = 30

[providers.deepseek]
provider = "deepseek"
mode = "official"
api_base = "https://api.deepseek.com"
model = "deepseek-chat"
api_key_ref = "brain-ai/deepseek"
```

### 20.3 依赖清单汇总

| 库 | 用途 | M1 实测版本 / 状态 |
|---|---|---|
| MSVC | 编译器 | 14.50（VS 2026 Insiders） |
| CMake | 构建 | 4.4.3（系统安装，生成器 `Visual Studio 18 2026`） |
| vcpkg | 包管理 | 2024-04 快照（`C:\dev\vcpkg`，基线 `eb0f108`） |
| Dear ImGui | UI | 1.90.7 (docking) ✅ |
| imgui-node-editor | 节点画布 | v0.9.3 源码集成 ✅（含最小补丁） |
| nlohmann/json | JSON | 3.11.3 ✅ |
| stb_image | 图片加载 | ✅（M5 使用） |
| nativefiledialog-extended | 文件对话框 | ✅ 1.3.0（vcpkg 快照中缺失 → 已补 **overlay port** `source/ports/nativefiledialog-extended`，随 `install-deps.cmd` 安装；工程 `find_package(nfd CONFIG REQUIRED)` + `nfd::nfd`） |
| spdlog | 日志 | 1.14.1 ✅ |
| toml++ | 配置 | 3.4.0 ✅（端口只提供库与 pkg-config，工程内手工建导入目标） |
| cpp-httplib | HTTP 客户端 | 0.15.3（openssl + brotli） ✅ |
| OpenSSL | SHA3 / TLS | 3.3.1 ✅ |
| WebView2 | 嵌入浏览器 | 1.0.2277.86（Runtime 152.0.4191.66） ✅ |
| GLFW | 窗口 / 输入 | 3.4 ✅ |
| glm | 数学（可选） | ✅（未使用） |
| Font Awesome | 图标（可选） | 待 M3 评估 |

### 20.4 示例工作流（6 个）

| 编号 | 名称 | 节点数 |
|---|---|---|
| E-01 | 文本续写 | 3 |
| E-02 | 图片转小说 | 5 |
| E-03 | 多图场景 | 6 |
| E-04 | 模板化生成 | 5 |
| E-05 | 双后端对比 | 7 |
| E-06 | 多段组合 | 5–6 |

---

**文档结束。**
**版本：v1.0**
**状态：MVP 设计完成，可进入 Phase 0 开发。**