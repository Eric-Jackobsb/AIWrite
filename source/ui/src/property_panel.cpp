
#include "property_panel.h"

#include "panel_internal.h"

#include "connection_test.h"   // M_patchAB（PB2-07 / PB2-30②）：面板诊断按钮
#include "node_registry.h"
#include "provider_resolve.h"
#include "editor_controller.h"
#include "output_panel.h"
#include "text_view.h"
#include "texture_cache.h"
#include "asset_store.h"
#include "file_dialog.h"
#include "log.h"
#include "paths.h"
#include "prompt_generator.h" // M_8nodes_extension L1-d（§4.3）：PromptTemplate「生成提示词」异步

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>   // L1-c（§4.1）：N-01 文件导入
#include <iterator>  // L1-c（§4.1）：istreambuf_iterator
#include <string>
#include <vector>

#include "property_panel_detail.h"  // ARCH-Z1：模块内契约（运行状态 / 重跑 / 批量操作段）

namespace aiwrite::ui {
namespace {
using engine::Node;
using engine::Param;
using engine::ParamType;
std::string param_error(const Param& param)
{
    std::string error;
    engine::Graph::validateParam(param, &error);
    return error;
}
// P7a-15：「输入」分组的端口连通清单
//  * 判据与运行前校验一致：非 optional 且连接数 0 → 必填未连（红字）
//  * 变长端口显示已连条数（P7a-01 起「图片理解」的 image 为变长）
// ARCH-R3：渲染读取走 `const engine::Graph&`（调用方从 view 传入）
void draw_input_port_summary(const Node& node, const engine::Graph& graph)
{
    if (node.inputs.empty()) {
        ImGui::TextDisabled("本节点没有输入端口");
        return;
    }
    ImGui::Separator();
    ImGui::TextDisabled("输入端口（%d）", static_cast<int>(node.inputs.size()));
    for (const engine::Port& port : node.inputs) {
        const int   connections = graph.inputConnectionCount(node.id, port.id);
        std::string text        = port.display_name;
        if (port.is_variadic) {
            text += "（变长）";
        }
        if (connections > 0) {
            text += "：已连 " + std::to_string(connections) + " 条";
            if (port.is_variadic && connections > 1) {
                text += "（按顺序全部送入）";
            }
            ImGui::BulletText("%s", text.c_str());
        }
        else if (port.is_optional) {
            ImGui::TextDisabled("  · %s：未连接（可选）", text.c_str());
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::BulletText("%s：未连接（必填）", text.c_str());
            ImGui::PopStyleColor();
        }
    }
}
void help_marker(const std::string& description)
{
    if (description.empty()) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
std::string lower_copy(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}
// M_8nodes_extension L1-a（§3.4）：该参数在网页版（mode=web）模式下被**忽略** ⇒ 灰色标注
//  * 集合 = 文档 §3.4 表格中标「（网页版忽略）」的新参数 + response_format；
//  * temperature / max_tokens / top_p 的忽略提示已在 Console 侧（local_nodes.cpp 门控文案），UI 不重复标。
bool web_ignored_in_web(const std::string& param_id)
{
    return param_id == "top_k" || param_id == "stop" || param_id == "frequency_penalty" ||
           param_id == "presence_penalty" || param_id == "seed" || param_id == "response_format";
}
// M_8nodes_extension L1-d（§4.3）：PromptTemplate「生成提示词」按钮（异步）
//  * 结果写回 `template`（先压撤销快照，可撤销 —— 文档 §4.3 关键约束）
//  * 失败 → 状态栏 + Console（**不静默回退到 manual**）
void draw_prompt_generate_button(engine::Node& node, PropertyEditResult& result,
                                     EditorController& editor)
{
    engine::Param* template_param = node.findParam("template");
    ImGui::Separator();
    const bool busy = promptGenerationRunning();
    ImGui::BeginDisabled(busy || template_param == nullptr);
    if (ImGui::Button(busy ? "正在生成提示词…" : "生成提示词", ImVec2(-FLT_MIN, 0.0f))) {
        // 节点参数**快照**（生成线程只读，不碰 UI 线程的 graph）
        nlohmann::json params = nlohmann::json::object();
        for (const engine::Param& param : node.params) {
            params[param.id] = param.value;
        }
        requestPromptGeneration(node.id, params);
        log::info("[生成提示词] " + node.id + " 已发起（官方 / 兼容 API）");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("把「模板」作为给 AI 的指令 → 生成结果**写入模板输入框**（可继续编辑）；\n"
                          "用节点内「提供商 / API Key」配置（official）；网页版暂不支持（按钮会提示）");
    }

    // 每帧消费生成结果 → 写回 template（先压撤销快照，可撤销）
    std::string gen_result;
    std::string gen_error;
    if (promptGenerationConsume(node.id, &gen_result, &gen_error)) {
        if (!gen_result.empty() && template_param != nullptr) {
            editor.snapshot("AI 生成提示词"); // 先压快照：可撤销（文档 §4.3）
            template_param->value = gen_result;
            result.changed        = true;
            editor.setStatus("已把 AI 生成结果写入「模板」（可撤销）");
            log::info("[生成提示词] 已写入 " + node.id + ".template（" +
                      std::to_string(gen_result.size()) + " 字节）");
        }
        else {
            editor.setStatus("生成提示词失败：" + gen_error);
            log::warn("[生成提示词] " + gen_error);
        }
    }
}
} // namespace
void draw_property_panel(const char* window_title, bool* open, Node* node,
                         const ReadOnlyView& view, PropertyEditResult& result,
                         EditorController& editor)
{
    if (open != nullptr && !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(380.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(window_title, open)) {
        ImGui::End();
        return;
    }

    if (node == nullptr) {
        ImGui::TextDisabled("未选中节点");
        ImGui::Separator();
        ImGui::TextWrapped("在画布中单击一个节点即可编辑其参数；"
                           "Ctrl+单击 或框选可多选，右键菜单可复制 / 删除。");
        ImGui::End();
        return;
    }

    engine::registerAllNodes();
    const engine::Definition* definition = engine::NodeRegistry::instance().find(node->type);

    // ---- P7a-15：三组可折叠（输入 / 参数 / 运行状态）----
    //  * 折叠状态由 ImGui 持久化到 `io.IniFilename`（app.cpp 已指向数据目录）→ **跨运行保留**
    if (ImGui::CollapsingHeader("输入", ImGuiTreeNodeFlags_DefaultOpen)) {
        // ---- 概览 ----
        ImGui::Text("节点 %s", node->id.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", node->type.c_str());
        ImGui::TextDisabled("分类: %s    状态: %s",
                            definition != nullptr ? engine::categoryName(definition->category) : "未知",
                            engine::nodeStateName(node->state));

        ImGui::TextUnformatted("标题");
        if (editor.request_focus_title) { // 双击节点 / 右键"改名" → 自动聚焦
            ImGui::SetKeyboardFocusHere();
            editor.request_focus_title = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool title_touched = ImGui::InputTextWithHint("##title", "节点标题", &node->title);
        if (title_touched || ImGui::IsItemDeactivatedAfterEdit()) {
            result.renamed = true;
        }
        if (ImGui::IsItemActivated()) {
            result.begin_edit = true; // 改名前的快照
        }

        if (definition != nullptr && !definition->description.empty()) {
            ImGui::TextWrapped("%s", definition->description.c_str());
        }

        // P7a-15：输入端口连通清单（必填未连 → 红字）
        draw_input_port_summary(*node, view.graph());
    }

    // ---- P7a-15：「参数」分组（可折叠）----
    //  * 分组体为既有代码，**保持原缩进**以便本次改动可审（纯格式差异，功能不受影响）
    if (ImGui::CollapsingHeader("参数", ImGuiTreeNodeFlags_DefaultOpen)) {
    // ---- 生效提供商（P1-a）：provider 输入优先，覆盖节点自身参数 ----
    // 解决"改了节点「模式」却不生效"的困惑；official（官方 API）与图片理解（M5-02）给出红字与提示
    if (engine::usesProvider(node->type)) {
        const engine::EffectiveProvider effective =
            engine::resolveEffectiveProvider(view.graph(), *node);
        if (node->type == "LLMGenerate") {
            if (effective.from_edge) {
                ImGui::TextDisabled("生效：%s（来自 提供商配置 %s）· 模型 %s", effective.mode.c_str(),
                                    effective.source_node.c_str(), effective.model.c_str());
            }
            else {
                ImGui::TextDisabled("生效：%s（节点自身设置；provider 输入未连接）",
                                    effective.mode.c_str());
            }
        }
        else if (node->type == "VLMGenerate") {
            // M5-02：图片理解只走 official（OpenAI 兼容多模态）；模型名来自「模型（自定义）」
            ImGui::TextDisabled("生效：图片理解 · 模式 %s · 模型 %s%s", effective.mode.c_str(),
                                effective.model.empty() ? "（未设置）" : effective.model.c_str(),
                                effective.from_edge ? "" : "（provider 输入未连接）");
        }

        // ---- M8-37（零开关）：生成字段未回填**不是**失败 —— 运行期自动识别选择器 ----
        if (effective.mode == "web" && aiwrite::ai::webLoginOnly(effective.spec)) {
            ImGui::TextDisabled("选择器未回填：运行时会**自动识别**输入框 / 答案容器（结果打印在 Console）；"
                                "如需固化为数据，见 --web-dom-dump");
        }
        const std::string reason = engine::unwiredReason(view.graph(), *node);
        if (!reason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s：本次运行该节点必定失败，下游会被跳过。", reason.c_str());
            ImGui::PopStyleColor();

            // 「已接线」提示与一键切换只针对文本生成（图片理解没有网页版入口，见 M5-02）
            if (node->type == "LLMGenerate" && engine::officialNotWired(effective)) {
                ImGui::TextDisabled(
                    "网页版已接线（provider.mode = web）：切换后即可真实生成，需先登录一次。");

                const std::string target_id = effective.from_edge ? effective.source_node : node->id;
                const std::string target_label =
                    effective.from_edge ? "提供商配置 " + target_id : std::string("本节点");
                if (ImGui::Button(("把 " + target_label + " 改为 web").c_str())) {
                    if (engine::Node* target = editor.model().graph.findNode(target_id)) {
                        if (engine::Param* mode = target->findParam("mode")) {
                            editor.snapshot("切换提供商为网页版"); // 先压快照：可撤销
                            mode->value = std::string("web");
                            editor.setStatus(target_label + " 已切换为网页版（可撤销）");
                            log::info("[参数面板] " + target_label + " mode → web");
                        }
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("网页版走 DeepSeek 网页版会话（需已登录一次）；\n"
                                      "官方 API（API Key）已接线（PB-05）：填 Key 即可用");
                }
            }
        }
    }

    // ---- 校验汇总（设计 §6.3；条件隐藏的参数不参与校验）----
    std::vector<std::string> errors;
    for (const Param& param : node->params) {
        if (!engine::paramVisible(*node, param)) {
            continue;
        }
        const std::string error = param_error(param);
        if (!error.empty()) {
            errors.push_back(param.display_name + ": " + error);
        }
    }
    if (errors.empty()) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "参数校验通过");
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "参数校验：%d 项错误",
                           static_cast<int>(errors.size()));
        for (const std::string& error : errors) {
            ImGui::BulletText("%s", error.c_str());
        }
    }

    ImGui::Separator();

    // ---- 参数控件（F2：搜索过滤 + 只读标记；条件隐藏的参数不显示）----
    static char param_filter[64] = {};
    ImGui::SetNextItemWidth(-72.0f);
    ImGui::InputTextWithHint("##param_filter", "搜索参数（id / 名称 / 说明）", param_filter,
                             sizeof(param_filter));
    ImGui::SameLine();
    if (ImGui::SmallButton("清除##param_filter")) {
        param_filter[0] = '\0';
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("清空搜索框（重新显示全部参数）");
    }
    const std::string filter_text = lower_copy(param_filter);
    const auto        matches_filter = [&filter_text](const Param& param) {
        if (filter_text.empty()) {
            return true;
        }
        return lower_copy(param.id).find(filter_text) != std::string::npos ||
               lower_copy(param.display_name).find(filter_text) != std::string::npos ||
               lower_copy(param.description).find(filter_text) != std::string::npos;
    };

    // ---- M_patchB L1 续（PB2-17）+ L2 修订（PB2-20）：ProviderConfig 自己的生效条目 ----
    //  * 按**该节点自身参数**解析（此前误用 resolve_effective_provider()：它对 ProviderConfig
    //    恒返回默认构造 → 站点区/提示永远回落内置默认，见 M_patchB §9.1）
    //  * **撤回**「web 条目自动锁 mode=web」（决策 D-21）：只提示、不静默改写
    const bool                is_provider_node = node->type == "ProviderConfig";
    engine::EffectiveProvider provider_effective;
    if (is_provider_node) {
        provider_effective = engine::resolveDisplayProvider(view.graph(), *node);
    }

    if (node->params.empty()) {
        ImGui::TextDisabled("该节点没有参数");
    }
    int hidden_params   = 0;
    int filtered_params = 0;
    int matched_params  = 0;
    // ---- M_8nodes_extension L1-a（§3.2）：参数**二级分组**渲染 ----
    //  * 预扫描「可见 + 命中搜索」的参数索引（保持声明顺序），再按分组排序；
    //  * 分组顺序固定：未分组 → 采样（折叠）→ 输出（展开）→ 高级（折叠）；
    //  * 同组连续渲染，组间输出二级 CollapsingHeader（折叠状态由 ImGui 持久化到 imgui.ini）。
    const auto group_rank = [](const std::string& group) {
        if (group == "sampling") {
            return 1;
        }
        if (group == "output") {
            return 2;
        }
        if (group == "advanced") {
            return 3;
        }
        return 0;
    };
    std::vector<std::size_t> render_order;
    for (std::size_t index = 0; index < node->params.size(); ++index) {
        const Param& param = node->params[index];
        if (!engine::paramVisible(*node, param)) {
            ++hidden_params;
            continue;
        }
        if (!matches_filter(param)) {
            ++filtered_params;
            continue;
        }
        ++matched_params;
        render_order.push_back(index);
    }
    std::stable_sort(render_order.begin(), render_order.end(),
                     [&](std::size_t a, std::size_t b) {
                         return group_rank(node->params[a].group) < group_rank(node->params[b].group);
                     });
    std::string active_group;
    bool        subgroup_open = true;
    for (std::size_t index : render_order) {
        Param&             param = node->params[index];
        const std::string  group = param.group;
        if (group != active_group) {
            active_group  = group;
            subgroup_open = true;
            if (!group.empty()) {
                const char* title = group == "sampling" ? "采样"
                                    : group == "output" ? "输出"
                                                        : "高级";
                const ImGuiTreeNodeFlags flags =
                    group == "output" ? ImGuiTreeNodeFlags_DefaultOpen : 0;
                subgroup_open = ImGui::CollapsingHeader(
                    (std::string(title) + "##subgroup_" + group).c_str(), flags);
            }
        }
        if (!group.empty() && !subgroup_open) {
            continue; // 该二级组折叠：不渲染组内参数控件（计数已在上面统计）
        }
        ImGui::PushID(param.id.c_str());
        ImGui::TextUnformatted(paramLabel(param).c_str());
        if (param.is_secret) {
            ImGui::SameLine();
            ImGui::TextDisabled("（密钥：仅存内存、不写盘）");
        }
        if (param.value != param.default_value) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "已改");
        }
        // M_8nodes_extension L1-a（§3.4）：网页版模式下被忽略的参数 → 灰色「（网页版忽略）」标注
        //  * 参数**仍可见**（不隐藏、不序列化改动）；temperature/max_tokens/top_p 的提示在 Console 侧
        if (isWebMode(*node) && web_ignored_in_web(param.id)) {
            ImGui::SameLine();
            ImGui::TextDisabled("（网页版忽略）");
        }
        help_marker(param.description);
        ImGui::PopID();

        // M_patchB L2 修订（PB2-20 / 决策 D-21）：ProviderConfig 的「模式」**恒为 {official, web}**
        //  * **不再**按条目 kind 裁剪候选 / 覆盖下拉项（不变量 I13：两项始终可选）
        //  * 与条目类型不一致时只给**橙色提示**（mode_kind_hint），绝不静默改写用户选择
        bool param_begin_edit  = false;
        bool param_changed_now = false;
        const bool param_edit_ended =
            draw_param_widget(*node, param, param_begin_edit, param_changed_now);
        // 只在「模式 = official」时提示（web 模式下的站点问题由站点区的**错误块**给出，避免重复；PB2-22）
        if (is_provider_node && param.id == "mode" && param.text() != "web") {
            const std::string hint = engine::modeKindHint(provider_effective);
            if (!hint.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.25f, 1.0f), "  ⚠ %s", hint.c_str());
            }
        }
        // 「切换提供商」→ 带出一次「模式」建议值（web 条目建议 web / official 条目建议 official）
        //  * 只带出一次建议：之后用户可任意改选，**不再**被改写（D-21）
        if (is_provider_node && param.id == "provider" && (param_changed_now || param_edit_ended)) {
            if (engine::Param* mode_param = node->findParam("mode")) {
                const std::string suggestion = engine::providerModeSuggestion(*node);
                if (!suggestion.empty() && mode_param->text() != suggestion) {
                    result.begin_edit = true; // 外层据此压一次撤销快照（可撤销）
                    mode_param->value = suggestion;
                    result.changed    = true;
                    log::info("参数变更: " + node->id + ".mode → " + suggestion +
                              "（切换提供商带出的建议值，可再改回）");
                }
            }
            // ---- M_8nodes_extension L1-f（§4.5）：参数预设带出 + 模型列表刷新 ----
            //  * 只在切换时**填空项**（不覆盖用户已填的值；D-21 同款「只带出一次」语义）
            //  * 模型枚举 = 生效条目 `models` 字段（**从配置表读，不从 API 拉**）；
            //    空数组 = 模型自由填写（保持原枚举不刷新）
            const ai::ProviderSpec* spec = ai::providerSpecs().find(param.text());
            if (spec != nullptr) {
                if (engine::Param* base = node->findParam("api_base")) {
                    if (base->text().empty()) {
                        base->value = ai::resolveApiBase(*spec, std::string());
                        result.changed = true;
                        log::info("参数变更: " + node->id + ".api_base → " + base->text() +
                                  "（切换提供商带出的默认地址）");
                    }
                }
                if (engine::Param* ref = node->findParam("api_key_ref")) {
                    if (ref->text().empty()) {
                        ref->value = ai::resolveKeyRef(*spec, std::string());
                        result.changed = true;
                        log::info("参数变更: " + node->id + ".api_key_ref → " + ref->text() +
                                  "（切换提供商带出的默认引用名）");
                    }
                }
                if (engine::Param* model = node->findParam("model")) {
                    std::vector<std::string> ids;
                    for (const ai::ProviderModelSpec& m : spec->models) {
                        ids.push_back(m.id);
                    }
                    if (!ids.empty()) {
                        model->enum_options = ids; // 模型列表刷新（表驱动）
                        if (std::find(ids.begin(), ids.end(), model->text()) == ids.end()) {
                            model->value = ids.front();
                            result.changed = true;
                            log::info("参数变更: " + node->id + ".model → " + model->text() +
                                      "（切换提供商带出的首选模型）");
                        }
                    }
                }
            }
        }
        if (param_changed_now || param_edit_ended) {
            result.changed = true; // 值已写回模型（编辑过程中即时生效）
        }
        if (param_edit_ended) {
            log::info("参数变更: " + node->id + "." + param.id); // 每段编辑只记一次
        }
        if (param_begin_edit) {
            result.begin_edit = true;
        }

        const std::string error = param_error(param);
        if (!error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "  %s", error.c_str());
        }
        ImGui::Spacing();
    }

    if (hidden_params > 0) {
        ImGui::TextDisabled("（%d 个 %s 专属参数已按当前模式隐藏）", hidden_params,
                            node->type == "ProviderConfig" ? "官方 API" : "条件");
    }
    if (!filter_text.empty()) {
        ImGui::TextDisabled("搜索「%s」：命中 %d 项，已过滤 %d 项（共 %d 项）", param_filter,
                            matched_params, filtered_params, matched_params + filtered_params);
    }

    // M_8nodes_extension L1-d（§4.3）：PromptTemplate「生成提示词」按钮（异步 · 结果写回模板）
    if (node->type == "PromptTemplate") {
        draw_prompt_generate_button(*node, result, editor);
    }

    // ---- 网页版（web 模式）：会话状态 + 登录入口（PB2-17/18/19）----
    if (node->type == "ProviderConfig") {
        if (isWebMode(*node)) {
            if (drawWebSessionSection(*node, view.graph())) {
                result.begin_edit = true; // 先压快照（可撤销）
                result.changed    = true;
            }
        }
        else {
            ImGui::Spacing();
            if (provider_effective.is_web()) {
                ImGui::TextDisabled("该提供商是网页版条目（建议「模式」= web）：把「模式」切到 web "
                                    "即可在此处打开该站点的登录窗口。");
            }
            else {
                ImGui::TextDisabled("提示：把「模式」切到 web 需要**网页版站点条目**"
                                    "（如 deepseek-web，或自建站点条目 —— 见使用说明 §9）；"
                                    "非网页版条目**不会**回落到内置默认站点（决策 D-22②）。");
            }
        }
    }
    } // P7a-15：「参数」分组结束

    // ---- M_patchAB §3.2 / §3.4（`PB2-07` / `PB2-30②`）：面板诊断按钮（异步 · 只读）----
    //  * 「测试连接」= 配置表离线断言 + 条目诊断（API：有 Key 时一条 ping；web：登录态与端点一致性）
    //  * 「测试选择器」= 复用只读探测 `ai::dom_site_probe()`（仅 `adapter=dom` 条目）
    //  * 结果：明细 → Console（`log::info`）+ 本区；一行结论 → 状态栏（主线程写）
    {
        ImGui::Separator();
        // 生效条目：本区在「参数」分组**之外**，故就地再解析一次（`resolve_display_provider` 是纯函数）
        const engine::EffectiveProvider diag_effective =
            engine::resolveDisplayProvider(view.graph(), *node);
        const std::string diag_provider =
            diag_effective.provider.empty() ? std::string() : diag_effective.provider;
        ImGui::TextUnformatted("诊断（只读 · 异步）");
        ImGui::TextDisabled("条目：%s（kind %s）",
                            diag_provider.empty() ? "(未解析)" : diag_provider.c_str(),
                            diag_effective.kind.empty() ? "?" : diag_effective.kind.c_str());

        const bool busy = diagnosticRunning(); // 任一诊断在跑 ⇒ 两个按钮都禁用（互斥）
        ImGui::BeginDisabled(busy || diag_provider.empty());
        if (ImGui::Button("测试连接（表校验 + 登录态 / 连通性）", ImVec2(-FLT_MIN, 0.0f))) {
            log::info("[参数面板] 触发「测试连接」（条目 " + diag_provider + "）");
            requestConnectionCheck(diag_provider, editor);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("API 条目：配置表断言 + 有 Key 时发一条 ping（「请只回复 pong」）；\n"
                              "网页版条目：登录态 + 生效端点一致性（**不发送任何提示词**）；\n"
                              "码语义与 CLI 一致：0 通过 / 1 失败 / 2 无 Key 或未登录");
        }

        const bool dom_entry = diag_effective.kind == "web" && diag_effective.spec != nullptr &&
                               diag_effective.spec->web.adapter == "dom";
        ImGui::BeginDisabled(busy || !dom_entry);
        if (ImGui::Button("测试选择器（只读诊断）", ImVec2(-FLT_MIN, 0.0f))) {
            log::info("[参数面板] 触发「测试选择器」（条目 " + diag_provider + "）");
            requestSelectorCheck(diag_provider, editor);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(dom_entry
                                  ? "复用只读探测：不写入页面、不发送内容、不读任何值；\n"
                                    "显示命中数 / 登录判据 / 修复建议（并写入 Console）"
                                  : "仅对 `adapter=dom` 的网页版条目可用（其它条目没有站点选择器）");
        }

        if (busy) {
            ImGui::TextDisabled("正在诊断：%s…（界面保持响应）", diagnosticTitle().c_str());
        }
        const std::string diag_report_text = diagnosticReport();
        if (!diag_report_text.empty()) {
            const int   code    = diagnosticCode();
            const char* verdict = code == 0 ? "通过"
                                            : (code == 2 ? "未就绪 / 缺 Key"
                                                         : (code == 3 ? "需人工登录" : "失败"));
            ImGui::TextDisabled("%s · 最近结果（%s）：%s", diagnosticTitle().c_str(), verdict,
                                diagnosticSummary().c_str());
            if (ImGui::CollapsingHeader(("诊断明细##diag_" + node->id).c_str())) {
                ImGui::TextUnformatted(diag_report_text.c_str());
            }
        }
    }

    // M5-01 收尾：图片输入的尺寸摘要（只读文件头，不需要 GL 上下文）
    // P7a-04/07：先解析资源令牌 → 真实路径；缺失时给可操作告警（不再只说「读不到文件头」）
    if (node->type == "ImageInput") {
        const engine::Param*           path_param = node->findParam("path");
        const std::vector<std::string> entries =
            path_param != nullptr ? paths::splitPathList(path_param->text())
                                  : std::vector<std::string>();
        if (!entries.empty()) {
            bool              missing = false;
            std::string       resolve_error;
            const std::string image_path = asset::toLocalPath(entries.front(), &missing, &resolve_error);
            if (missing || image_path.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s",
                                   resolve_error.empty() ? "图片资源缺失（可能已被删除）"
                                                         : resolve_error.c_str());
                ImGui::PopStyleColor();
            }
            else {
                int         width  = 0;
                int         height = 0;
                std::string error;
                if (imageSize(image_path, &width, &height, &error)) {
                    if (entries.size() > 1) {
                        ImGui::TextDisabled("图片尺寸：%d×%d（共 %d 张，此处显示第 1 张）", width,
                                            height, static_cast<int>(entries.size()));
                    }
                    else {
                        ImGui::TextDisabled("图片尺寸：%d×%d", width, height);
                    }
                }
                else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                    ImGui::TextWrapped("%s", error.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
    }

    // ARCH-Z1：尾部「运行状态 + 重跑 + 底部批量操作」整段已搬入 property_panel_runtime.cpp
    //（同一 Begin/End 之内、同一帧、同一插入点 ⇒ 渲染顺序与副作用顺序不变）
    draw_property_panel_runtime(node, view, result, editor);
}
} // namespace aiwrite::ui
