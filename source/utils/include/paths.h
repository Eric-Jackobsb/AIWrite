#pragma once

#include <filesystem>
#include <string>
#include <vector>

// 路径工具：统一管理 ~/.brain-ai 数据目录（设计文档 11.1 节）
namespace aiwrite::paths {

// 用户主目录（%USERPROFILE%）
const std::filesystem::path& home();

// 展开以 "~" 开头的路径；其他路径原样返回
std::filesystem::path expand(const std::string& value);

// 数据根目录 ~/.brain-ai
const std::filesystem::path& dataRoot();
const std::filesystem::path& outputsDir();       // ~/.brain-ai/outputs
const std::filesystem::path& workflowsDir();     // ~/.brain-ai/workflows
const std::filesystem::path& snapshotsDir();     // ~/.brain-ai/snapshots
// CN-实施（Composition_node）：上级节点独立文件存储目录（每规格一文件，见设计文档 §9.3）
// PS-D9：存储双源 —— composites_dir() 仅作旧目录迁移来源（首次 reload 并入用户源后删除）
const std::filesystem::path& compositesDir();    // ~/.brain-ai/composites（旧目录 · 迁移来源）
const std::filesystem::path& builtinCompositesDir(); // <source>/workflows/compositeNodes（内置 · 只读）
const std::filesystem::path& userCompositesDir();    // ~/.brain-ai/workflows/compositeNodes（用户 · 可读写）
const std::filesystem::path& logsDir();          // ~/.brain-ai/logs
const std::filesystem::path& configFile();       // ~/.brain-ai/config.toml
const std::filesystem::path& appLogFile();      // ~/.brain-ai/logs/app.log
const std::filesystem::path& workflowLogFile(); // ~/.brain-ai/logs/workflow.log
const std::filesystem::path& recentFile();       // ~/.brain-ai/recent.json
const std::filesystem::path& webview2_profile();  // ~/.brain-ai/webview2（批 5 退役）
// M7B：Pydoll **单 profile**（`M7B-01` 实测落点；与 Python `browsers.PROFILE` 同值）
const std::filesystem::path& pydollProfile();    // ~/.brain-ai/pydoll-profile
// MB-D0-8 L2：登录态加密快照目录（**≠ `snapshots_dir()`** —— 那个是工作流执行快照）
//  * 快照本体（DPAPI 密文）由 Python `session.py` 读写；C++ 侧只用它做「路径 / 状态展示」
//  * **不**加入 `ensure_data_dirs()`（不留空目录误导；由 Python 侧按需创建）
const std::filesystem::path& sessionSnapshotDir(); // ~/.brain-ai/session
// P7a-04：统一资源目录（图片归档根）—— 工作流只存令牌，文件全部收在这里
const std::filesystem::path& assetsImagesDir(); // ~/.brain-ai/assets/images

// 可执行文件所在目录
const std::filesystem::path& exeDir();
std::filesystem::path assetsDir();               // <exe_dir>/assets

// ---- Provider 配置表（M_patchB L1 / PB2-02）----
std::filesystem::path providersAssetFile();     // <exe_dir>/assets/providers.json
std::filesystem::path userProvidersDir();       // ~/.brain-ai/providers.d（用户新增条目）
std::filesystem::path userProvidersFile();      // ~/.brain-ai/providers.json（字段级覆盖）

// 创建全部数据目录；返回失败个数（0 = 全部就绪）
int ensureDataDirs();

// ---- 多值路径串（P7a-02 / M7：图片输入支持多选）----
//  * 分隔符 = **换行**（Windows 路径的合法字符不含 \n，因此不需要转义）
//  * 去空行 / 去首尾空白（含 \r，兼容 CRLF）/ 保序 / **去重**（同一路径只保留首次出现）
//  * 单值输入（无换行）→ 长度为 1 的列表 = **旧工作流语义不变**
std::vector<std::string> splitPathList(const std::string& value);

// 路径列表 → 多值路径串（每行一个；空列表返回空串）
std::string joinPathList(const std::vector<std::string>& paths);

} // namespace aiwrite::paths
