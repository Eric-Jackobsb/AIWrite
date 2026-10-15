#include "node_registry.h"

#include "provider_spec.h" // M_patchB L1：提供商列表 / 默认值来自配置表
#include "config.h"        // M_8nodes_extension L1-a：retry_enabled 默认值跟随全局 error.retry_enabled

#include <algorithm>
#include <utility>

namespace aiwrite::engine {
namespace {

// ------------------------------------------------------------ 构造小工具 ----
Port input_port(const std::string& id, const std::string& name, PortType type,
                bool variadic = false, bool optional = false)
{
    Port port;
    port.id           = id;
    port.name         = name;
    port.display_name = name;
    port.type         = type;
    port.direction    = PortDirection::Input;
    port.is_variadic  = variadic;
    port.is_optional  = optional;
    return port;
}

Port output_port(const std::string& id, const std::string& name, PortType type)
{
    Port port;
    port.id           = id;
    port.name         = name;
    port.display_name = name;
    port.type         = type;
    port.direction    = PortDirection::Output;
    return port;
}

Param base_param(const std::string& id, const std::string& name, ParamType type)
{
    Param param;
    param.id           = id;
    param.name         = name;
    param.display_name = name;
    param.type         = type;
    return param;
}

Param text_param(const std::string& id, const std::string& name, const std::string& default_value,
                 ParamType type = ParamType::Text, bool required = false, bool secret = false,
                 const std::string& description = {})
{
    Param param         = base_param(id, name, type);
    param.default_value = default_value;
    param.value         = default_value;
    param.is_required   = required;
    param.is_secret     = secret;
    param.description   = description;
    return param;
}

Param int_param(const std::string& id, const std::string& name, int default_value, double min_value,
                double max_value, const std::string& description = {})
{
    Param param         = base_param(id, name, ParamType::Int);
    param.default_value = default_value;
    param.value         = default_value;
    param.min_value     = min_value;
    param.max_value     = max_value;
    param.step          = 1.0;
    param.description   = description;
    return param;
}

Param float_param(const std::string& id, const std::string& name, double default_value,
                  double min_value, double max_value, double step,
                  const std::string& description = {})
{
    Param param         = base_param(id, name, ParamType::Float);
    param.default_value = default_value;
    param.value         = default_value;
    param.min_value     = min_value;
    param.max_value     = max_value;
    param.step          = step;
    param.description   = description;
    return param;
}

Param bool_param(const std::string& id, const std::string& name, bool default_value,
                 const std::string& description = {})
{
    Param param         = base_param(id, name, ParamType::Bool);
    param.default_value = default_value;
    param.value         = default_value;
    param.description   = description;
    return param;
}

Param enum_param(const std::string& id, const std::string& name,
                 std::vector<std::string> options, const std::string& default_value,
                 const std::string& description = {})
{
    Param param         = base_param(id, name, ParamType::Enum);
    param.enum_options  = std::move(options);
    param.default_value = default_value;
    param.value         = default_value;
    param.description   = description;
    return param;
}

Param file_param(const std::string& id, const std::string& name, bool required,
                 const std::string& description = {})
{
    Param param         = base_param(id, name, ParamType::File);
    param.default_value = std::string();
    param.value         = std::string();
    param.is_required   = required;
    param.description   = description;
    return param;
}

Param directory_param(const std::string& id, const std::string& name, const std::string& default_value,
                      bool required = false, const std::string& description = {})
{
    Param param         = base_param(id, name, ParamType::Directory);
    param.default_value = default_value;
    param.value         = default_value;
    param.is_required   = required;
    param.description   = description;
    return param;
}

// M_8nodes_extension L1-a：StringList 参数（JSON 字符串数组，如 `stop` 序列）
//  * 编辑形态 = 逗号分隔（UI 层渲染）；空数组 = 未设置
Param string_list_param(const std::string& id, const std::string& name,
                        const std::vector<std::string>& default_values,
                        const std::string& description = {})
{
    Param param           = base_param(id, name, ParamType::StringList);
    param.default_value   = default_values;
    param.value           = default_values;
    param.description     = description;
    return param;
}

// M_8nodes_extension L1-a（§3.2 / §5.2）：参数二级分组标注
//  * "sampling"（采样，折叠）/ "output"（输出，展开）/ "advanced"（高级，折叠）
//  * 按值传参：接收 helper 返回的临时对象（右值），标注后原样返回
Param in_group(Param param, const std::string& group)
{
    param.group = group;
    return param;
}

// ============================================================================
//  M_8nodes_extension L1-a（§3.1 / §5.1）：**推理节点公共参数面**
//
//   N-06（LLMGenerate）/ N-07（VLMGenerate）**注册同一份**公共参数，
//   只在各自特有参数上做增量（N-06 特有 `model`；N-07 无特有）。
//   * **不改动**参数 id / 默认值 / 范围 / 枚举（旧工作流 JSON 兼容）；
//   * `mode` 默认值由调用方传入（`NE-D38`：N-06 保持现状 = web，N-07 = official，
//     两处暂不统一）；seed 范围上限 = IM_S32_MAX/2（参数面板 SliderInt 断言上限）；
//   * 分组归属见 §5.2：采样（折叠）/ 输出（展开）/ 高级（折叠）；
//   * 超时 / 重试覆盖项（§3.3 / §5.3）：0 / 空 = 用全局配置，非零 = 节点优先。
// ============================================================================
std::vector<Param> inference_common_params(const std::string& mode_default)
{
    std::vector<Param> params;

    // ---- 输出组（展开）：system_prompt · mode · max_tokens · stop · stream ----
    params.push_back(in_group(text_param("system_prompt", "系统提示词", std::string(), ParamType::Text),
                              "output"));
    params.push_back(in_group(enum_param("mode", "模式", {"official", "web"}, mode_default,
                                         "official = 官方 API（API Key）；web = 网页版（需先登录一次）"),
                              "output"));
    params.push_back(in_group(int_param("max_tokens", "最大长度", 2048, 1.0, 8192.0,
                                        "生成的最大 token 数"),
                              "output"));
    params.push_back(in_group(
        string_list_param("stop", "停止序列", {},
                          "遇到这些字符串即停止生成（每项一个，逗号分隔；网页版忽略）"),
        "output"));
    params.push_back(in_group(bool_param("stream", "流式输出", true, "M4 实现流式回调"), "output"));

    // ---- 采样组（折叠）：temperature · top_p · top_k · frequency_penalty · presence_penalty · seed ----
    params.push_back(in_group(float_param("temperature", "温度", 0.7, 0.0, 2.0, 0.1,
                                          "采样温度，越高越随机"),
                              "sampling"));
    params.push_back(in_group(float_param("top_p", "Top P", 1.0, 0.0, 1.0, 0.05), "sampling"));
    params.push_back(in_group(int_param("top_k", "Top K", 0, 0.0, 100.0,
                                        "只从概率最高的前 K 个候选中采样；0 = 不限制（网页版忽略）"),
                              "sampling"));
    params.push_back(in_group(float_param("frequency_penalty", "频率惩罚", 0.0, -2.0, 2.0, 0.1,
                                          "已出现 token 的重复惩罚；负值 = 鼓励重复（网页版忽略）"),
                              "sampling"));
    params.push_back(in_group(float_param("presence_penalty", "存在惩罚", 0.0, -2.0, 2.0, 0.1,
                                          "对出现过（无论次数）的 token 惩罚；负值 = 鼓励重复（网页版忽略）"),
                              "sampling"));
    params.push_back(in_group(int_param("seed", "随机种子", 0, 0.0, 1000000000.0,
                                        "0 = 不指定；点「重新生成（新 seed）」会写入新值"
                                        "（官方 API 随请求发送，网页版忽略）"),
                              "sampling"));

    // ---- 高级组（折叠）：timeout.* / error.* 节点级覆盖（0 / 空 = 用全局配置）----
    params.push_back(in_group(int_param("timeout_connect_ms", "连接超时(ms)", 0, 0.0, 3600000.0,
                                        "0 = 用全局 timeout.connect_ms；非零 = 节点优先"),
                              "advanced"));
    params.push_back(in_group(int_param("timeout_first_byte_ms", "首字节超时(ms)", 0, 0.0, 3600000.0,
                                        "0 = 用全局 timeout.first_byte_ms；非零 = 节点优先"),
                              "advanced"));
    params.push_back(in_group(int_param("timeout_stream_idle_ms", "流式空闲超时(ms)", 0, 0.0, 3600000.0,
                                        "0 = 用全局 timeout.stream_idle_ms；非零 = 节点优先"),
                              "advanced"));
    params.push_back(in_group(bool_param("retry_enabled", "重试开关", aiwrite::app_config().error.retry_enabled,
                                         "仅配合下方 retry_count 使用：retry_count=0 时本项忽略（走全局）"),
                              "advanced"));
    params.push_back(in_group(int_param("retry_count", "重试次数", 0, 0.0, 5.0,
                                        "0 = 用全局 error.retry_count（上限 5）；非零 = 节点优先"),
                              "advanced"));
    params.push_back(in_group(int_param("retry_interval_ms", "重试间隔(ms)", 0, 0.0, 30000.0,
                                        "0 = 用全局 error.retry_interval_ms；非零 = 节点优先"),
                              "advanced"));
    return params;
}

} // namespace

// ---------------------------------------------------------------- 分类名称 ----
const char* categoryName(NodeCategory category)
{
    switch (category) {
    case NodeCategory::Input:     return "输入";
    case NodeCategory::Process:   return "处理";
    case NodeCategory::Config:    return "配置";
    case NodeCategory::Inference: return "推理";
    case NodeCategory::Output:    return "输出";
    case NodeCategory::Auxiliary: return "辅助功能"; // M_node_expansion L2
    }
    return "输入";
}

const char* categoryKey(NodeCategory category)
{
    switch (category) {
    case NodeCategory::Input:     return "input";
    case NodeCategory::Process:   return "process";
    case NodeCategory::Config:    return "config";
    case NodeCategory::Inference: return "inference";
    case NodeCategory::Output:    return "output";
    case NodeCategory::Auxiliary: return "auxiliary"; // M_node_expansion L2
    }
    return "input";
}

// ------------------------------------------------------------- NodeRegistry ----
NodeRegistry& NodeRegistry::instance()
{
    static NodeRegistry registry;
    return registry;
}

void NodeRegistry::registerNode(Definition definition)
{
    for (Definition& existing : definitions_) {
        if (existing.type == definition.type) {
            existing = std::move(definition); // 覆盖同名类型
            return;
        }
    }
    definitions_.push_back(std::move(definition));
}

const Definition* NodeRegistry::find(const std::string& type) const
{
    for (const Definition& definition : definitions_) {
        if (definition.type == type) {
            return &definition;
        }
    }
    return nullptr;
}

Definition* NodeRegistry::findMutable(const std::string& type)
{
    for (Definition& definition : definitions_) {
        if (definition.type == type) {
            return &definition;
        }
    }
    return nullptr;
}

std::vector<const Definition*> NodeRegistry::listTypes() const
{
    std::vector<const Definition*> result;
    result.reserve(definitions_.size());
    for (const Definition& definition : definitions_) {
        result.push_back(&definition);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const Definition* a, const Definition* b) {
                         if (a->category != b->category) {
                             return static_cast<int>(a->category) < static_cast<int>(b->category);
                         }
                         return a->display_name < b->display_name;
                     });
    return result;
}

std::vector<const Definition*> NodeRegistry::listByCategory(NodeCategory category) const
{
    std::vector<const Definition*> result;
    for (const Definition& definition : definitions_) {
        if (definition.category == category) {
            result.push_back(&definition);
        }
    }
    return result;
}

std::size_t NodeRegistry::size() const
{
    return definitions_.size();
}

void NodeRegistry::clear()
{
    definitions_.clear();
}

// ---------------------------------------------------------- 9 个 MVP 节点 -----
void registerAllNodes()
{
    NodeRegistry& registry = NodeRegistry::instance();
    if (registry.size() > 0) {
        return; // 幂等
    }

    // --- N-01 Text Input（输入，设计 §4.4 / §9.2）---------------------------
    {
        Definition definition;
        definition.type         = "TextInput";
        definition.display_name = "文本输入";
        definition.title        = "文本输入";
        definition.category     = NodeCategory::Input;
        definition.description  = "输出参数中的文本";
        definition.outputs      = {output_port("text", "文本", PortType::Text)};
        definition.params       = {text_param("text", "文本内容", std::string(), ParamType::Text,
                                              /*required*/ true, /*secret*/ false,
                                              "节点执行时输出的文本")};
        registry.registerNode(std::move(definition));
    }

    // --- N-02 Image Input ---------------------------------------------------
    {
        Definition definition;
        definition.type         = "ImageInput";
        definition.display_name = "图片输入";
        definition.title        = "图片输入";
        definition.category     = NodeCategory::Input;
        definition.description  = "输出图片路径（支持多张，每行一个），执行时逐张校验文件存在";
        definition.outputs      = {output_port("image", "图片", PortType::Image)};
        // M_8nodes_extension L1-c（§4.2）：图片目录（批量）+ 图片预处理（读取 → 裁剪 → 缩放 → 编码）
        //  * `path` 降为可选（与 `directory` 至少填一个）；旧工作流 JSON 兼容（id 不变）
        definition.params       = {
            file_param("path", "图片路径", /*required*/ false,
                       "支持 png / jpg / jpeg / bmp / webp；多张图每行一个路径（可用「多选…」一次加入，"
                       "P7a-02）；与「图片目录」至少填一个"),
            directory_param("directory", "图片目录", std::string(), false,
                            "批量取该目录下**所有**图片（按扩展名 + 内容嗅探过滤，文件名排序）；"
                            "与「图片路径」至少填一个"),
            enum_param("resize_mode", "缩放模式", {"none", "fit", "fill"}, "none",
                       "none = 不缩放；fit = 等比缩放到「最大宽/高」内；fill = 等比覆盖「最大宽/高」后居中裁剪"),
            int_param("resize_max_width", "最大宽", 0, 0.0, 100000.0, "缩放目标最大宽（0 = 不限制）"),
            int_param("resize_max_height", "最大高", 0, 0.0, 100000.0, "缩放目标最大高（0 = 不限制）"),
            bool_param("crop_enabled", "裁剪", false, "裁剪开关（按下方裁剪区域，相对原图）"),
            int_param("crop_x", "裁剪 X", 0, 0.0, 100000.0, "裁剪起点 X"),
            int_param("crop_y", "裁剪 Y", 0, 0.0, 100000.0, "裁剪起点 Y"),
            int_param("crop_w", "裁剪宽", 0, 0.0, 100000.0, "裁剪宽（0 = 到右侧剩余）"),
            int_param("crop_h", "裁剪高", 0, 0.0, 100000.0, "裁剪高（0 = 到下侧剩余）"),
        };
        registry.registerNode(std::move(definition));
    }

    // --- N-03 Prompt Template ----------------------------------------------
    {
        Definition definition;
        definition.type         = "PromptTemplate";
        definition.display_name = "提示词模板";
        definition.title        = "提示词模板";
        definition.category     = NodeCategory::Process;
        definition.description  = "把 {var} 替换为输入值";
        definition.inputs       = {input_port("vars", "变量", PortType::Any,
                                              /*variadic*/ true, /*optional*/ true)};
        definition.outputs      = {output_port("text", "文本", PortType::Text)};
        // M_8nodes_extension L1-d（§4.3）：AI 生成提示词 —— 配置全在节点参数内（复用 N-05 解析）
        //  * 默认 manual：`{var}` 占位替换（现有行为）；AI 生成 = 独立按钮（见参数面板）
        //  * `seed` 只作用于内部 AI 调用，不影响模板替换（VL1-06）
        //  * mode=web 时隐藏 official 专属参数（API 地址 / 模型 / Key / 引用名）—— 与 N-05 同规则
        definition.params       = [](const std::string& default_provider) {
            std::vector<Param> params;
            params.push_back(text_param("template", "模板", "请根据以下内容续写：\n{vars}",
                                        ParamType::Text, /*required*/ true, false,
                                        "用 {端口名} 引用输入值；变长输入按顺序可用 {1}/{2}…（单值时也可写 {vars}）"));
            params.push_back(enum_param("provider", "提供商",
                                        {default_provider}, default_provider,
                                        "AI 生成提示词用（来自配置表 assets/providers.json）"));
            params.push_back(enum_param("mode", "模式", {"official", "web"}, "official",
                                        "official = 官方 API（AI 生成）；web = 网页版（暂不支持生成，按钮会提示）"));
            params.push_back(text_param("api_base", "API 地址", std::string(), ParamType::String,
                                        false, false, "留空 = 该提供商默认地址"));
            params.push_back(enum_param("model", "模型", {"deepseek-chat", "deepseek-reasoner"},
                                        "deepseek-chat", "AI 生成提示词用的模型"));
            params.push_back(text_param("model_custom", "模型（自定义）", std::string(),
                                        ParamType::String, false, false, "覆盖上方枚举"));
            params.push_back(text_param("api_key", "API Key", std::string(), ParamType::String,
                                        false, true, "仅保存在内存中；填写后自动入库（凭据管理器）"));
            params.push_back(text_param("api_key_ref", "Key 引用名", std::string(), ParamType::String,
                                        false, false, "留空 = 按提供商自动生成"));
            params.push_back(text_param("system_prompt", "系统提示词（生成用）", std::string(),
                                        ParamType::Text, false, false,
                                        "引导 AI 生成提示词；留空 = 只用模板作为指令"));
            params.push_back(float_param("temperature", "温度", 0.7, 0.0, 2.0, 0.1,
                                         "生成提示词用采样温度"));
            params.push_back(int_param("max_tokens", "最大长度", 2048, 1.0, 8192.0,
                                       "生成提示词用最大 token 数"));
            params.push_back(float_param("top_p", "Top P", 1.0, 0.0, 1.0, 0.05));
            params.push_back(int_param("top_k", "Top K", 0, 0.0, 100.0, "0 = 不限制"));
            params.push_back(string_list_param("stop", "停止序列", {}, "逗号分隔"));
            params.push_back(int_param("seed", "随机种子（生成用）", 0, 0.0, 1000000000.0,
                                       "只作用于内部 AI 调用，不影响模板替换"));
            params.push_back(bool_param("stream", "流式输出", true));
            for (Param& param : params) {
                if (param.id == "api_base" || param.id == "model" || param.id == "model_custom" ||
                    param.id == "api_key" || param.id == "api_key_ref") {
                    param.visible_when_param = "mode";
                    param.visible_when_value = "official";
                }
            }
            return params;
        }(provider_option_ids().empty() ? std::string("deepseek") : provider_option_ids().front());
        registry.registerNode(std::move(definition));
    }

    // --- N-04 Text Merge ---------------------------------------------------
    {
        Definition definition;
        definition.type         = "TextMerge";
        definition.display_name = "文本合并";
        definition.title        = "文本合并";
        definition.category     = NodeCategory::Process;
        definition.description  = "合并多个文本输入，支持分隔符";
        definition.inputs       = {input_port("texts", "文本", PortType::Text,
                                              /*variadic*/ true)};
        definition.outputs      = {output_port("text", "文本", PortType::Text)};
        definition.params       = {text_param("separator", "分隔符", "\n", ParamType::String,
                                              false, false, "支持 \\n 等转义写法"),
                                   bool_param("ignore_empty", "忽略空输入", true),
                                   // ---- M_8nodes_extension L1-b（§4.4）：排序 / 前缀后缀 / 带源标签 / 去重 / 空行增强 ----
                                   enum_param("sort_mode", "排序", {"edge_order", "by_name"},
                                              "edge_order", "edge_order = 按输入边的顺序；by_name = 按内容字典序"),
                                   text_param("prefix", "每段前缀", std::string(), ParamType::String,
                                              false, false, "加在每段（非空段）前面；支持 \\n 等转义写法"),
                                   text_param("suffix", "每段后缀", std::string(), ParamType::String,
                                              false, false, "加在每段（非空段）后面；支持 \\n 等转义写法"),
                                   enum_param("source_label", "带源标签", {"none", "[nX]"}, "none",
                                              "none = 不标注；[nX] = 每段前加 [1]、[2]…（按合并后的顺序）"),
                                   bool_param("dedup", "去重", false, "相同内容只保留首次出现的那一份（保序）"),
                                   enum_param("empty_line_mode", "空行处理", {"keep", "collapse", "remove"},
                                              "keep", "keep = 保留空段（受「忽略空输入」控制）；collapse = 连续空段合并为一个；remove = 全部移除")};
        registry.registerNode(std::move(definition));
    }

    // --- N-05 Provider Config（配置）---------------------------------------
    {
        // M_patchB L1（PB2-05）：提供商列表来自**配置表**（official 在前、web 在后，顺序稳定）
        // PB2-24：口径抽到 `provider_option_ids()` —— 重载配置表后由
        //         `refresh_provider_options()` 就地重建枚举（此处与刷新**同一份**逻辑）
        std::vector<std::string> provider_ids = provider_option_ids();
        if (provider_ids.empty()) {
            provider_ids.push_back("deepseek"); // 极小概率（表不可用时 engine 侧兜底）
        }
        const std::string default_provider = provider_ids.front();

        Definition definition;
        definition.type         = "ProviderConfig";
        definition.display_name = "提供商配置";
        definition.title        = "提供商配置";
        definition.category     = NodeCategory::Config;
        definition.description  = "输出 provider 句柄（配置表驱动），校验配置完整性";
        definition.outputs      = {output_port("provider", "提供商", PortType::Provider)};
        definition.params       = {
            enum_param("provider", "提供商", provider_ids, default_provider,
                       "来自配置表 assets/providers.json；自定义条目放 ~/.brain-ai/providers.d/"),
            enum_param("mode", "模式", {"official", "web"}, "official",
                       "official = 官方 API（API Key）；web = 网页版（需先登录一次）。两项都可选"
                       "（网页版与官方 API 同等优先级）；切换提供商时会带出建议值，"
                       "与条目类型不一致时只给提示、不自动改写"),
            text_param("api_base", "API 地址", std::string(), ParamType::String, false, false,
                       "留空 = 用该提供商的默认地址（见配置表）；自定义条目请填完整前缀（含 /v1）"),
            enum_param("model", "模型", {"deepseek-chat", "deepseek-reasoner"}, "deepseek-chat",
                       "内置提示枚举；生效模型由配置表解析（「模型（自定义）」优先）"),
            // M5-02：模型名解锁 —— 留空用上方枚举；填第三方/本地模型名则覆盖
            // （DeepSeek 官方 API 无视觉模型，图片理解需指向兼容 VLM，如智谱 glm-4v-flash）
            text_param("model_custom", "模型（自定义）", std::string(), ParamType::String, false, false,
                       "留空 = 按配置表（该提供商的候选模型）；填写则覆盖（例：glm-4v-flash / Qwen/Qwen2.5-VL-72B-Instruct）"),
            text_param("api_key", "API Key", std::string(), ParamType::String,
                       /*required*/ false, /*secret*/ true,
                       "仅保存在内存中；填写后自动入库（凭据管理器，PB-06）"),
            text_param("api_key_ref", "Key 引用名", std::string(), ParamType::String,
                       false, false, "留空 = 按提供商自动生成（如 brain-ai/zhipu）；填了则优先"),
        };
        // mode=web 时隐藏 official 专属参数（API 地址 / 模型 / 自定义模型 / Key / 引用名）
        // 可见性判定见 engine::param_visible()：参数面板、画布预览与运行前校验共用同一份规则
        for (Param& param : definition.params) {
            if (param.id == "api_base" || param.id == "model" || param.id == "model_custom" ||
                param.id == "api_key" || param.id == "api_key_ref") {
                param.visible_when_param = "mode";
                param.visible_when_value = "official";
            }
        }
        registry.registerNode(std::move(definition));
    }

    // --- N-06 LLM Generate（推理，设计 §9.2 / M4-03）----------------------
    {
        Definition definition;
        definition.type         = "LLMGenerate";
        definition.display_name = "文本生成";
        definition.title        = "文本生成";
        definition.category     = NodeCategory::Inference;
        definition.description  = "读取 prompt + provider，调用后端生成文本";
        definition.inputs       = {input_port("prompt", "提示词", PortType::Text),
                                   input_port("provider", "提供商", PortType::Provider,
                                              false, /*optional*/ true)};
        definition.outputs      = {output_port("text", "文本", PortType::Text)};
        // M_8nodes_extension L1-a：公共参数面（system_prompt / mode / max_tokens / stop / stream
        //   + temperature / top_p / top_k / frequency_penalty / presence_penalty / seed
        //   + timeout.* / error.* 覆盖项），N-06 特有 `model` 追加在 mode 之后。
        //   `mode` 默认值**保持现状 = web**（NE-D38 · 用户拍板 ③A · 零行为变更）
        definition.params       = [](const std::string& mode_default) {
            std::vector<Param> params = inference_common_params(mode_default);
            const auto         pos    = std::find_if(params.begin(), params.end(),
                                         [](const Param& p) { return p.id == "mode"; });
            // 防御：公共面必然含 mode（inference_common_params 首段即注册）；万一被改丢则追加到末尾
            if (pos == params.end()) {
                params.push_back(enum_param("model", "模型",
                                            {"deepseek-chat", "deepseek-reasoner", "expert"},
                                            "deepseek-chat",
                                            "deepseek-reasoner 走网页版「深度思考」；expert 走专家模式"));
                return params;
            }
            // M_8nodes_extension L1-e（§4.6）：JSON 输出（response_format）+ 多候选 `n`
            //  * `n>1` 时 text 端口输出**第一个**候选；多候选输出端口 `texts` 留实施记录（待确认）
            //  * 网页版忽略标注集已含 response_format（§3.4）；`n` 对网页版同样忽略
            params.insert(pos + 1, {enum_param("model", "模型",
                                               {"deepseek-chat", "deepseek-reasoner", "expert"},
                                               "deepseek-chat",
                                               "deepseek-reasoner 走网页版「深度思考」；expert 走专家模式"),
                                    enum_param("response_format", "输出格式", {"text", "json_object"},
                                               "text",
                                               "json_object = 请求体带 response_format，后端返回 JSON 对象"
                                               "（豆包 / DeepSeek 支持度实施后实测；网页版忽略）"),
                                    int_param("n", "候选数", 1, 1.0, 8.0,
                                              "一次生成多个候选；text 端口输出第一个（网页版忽略）")});
            return params;
        }("web");
        registry.registerNode(std::move(definition));
    }

    // --- N-07 VLM Generate（推理，M5-02）----------------------------------
    {
        Definition definition;
        definition.type         = "VLMGenerate";
        definition.display_name = "图片理解";
        definition.title        = "图片理解";
        definition.category     = NodeCategory::Inference;
        definition.description  = "读取 prompt + 图片（单张或多张）+ provider，由视觉模型生成文字";
        definition.inputs       = {input_port("prompt", "提示词", PortType::Text),
                                   // P7a-01：图片端口改**变长** —— 多张图按边顺序全部送入（I19 兼容单张）
                                   input_port("image", "图片", PortType::Image, /*variadic*/ true),
                                   input_port("provider", "提供商", PortType::Provider,
                                              false, /*optional*/ true)};
        definition.outputs      = {output_port("text", "文本", PortType::Text)};
        // M_8nodes_extension L1-a：公共参数面（与 N-06 同一份，VL1-01 对称差集为空除 model）
        //   * `mode` / `seed` 为**新增**参数面（能力早已存在：M8-14 网页版图片理解已落地）——
        //     注册表参数面缺 `mode` / `seed` 才是真实缺口（批 9-0 P1-4 复核结论）
        //   * `mode` 默认 = official（保持 N-07 现状；NE-D38：两处默认值暂不统一）
        definition.params = [](const std::string& mode_default) {
            std::vector<Param> params = inference_common_params(mode_default);
            // M_8nodes_extension L1-b（§4.7）：多图顺序（图片预处理参数随 L1-c 的 N-02 一同补齐）
            params.push_back(enum_param("image_order", "多图顺序", {"edge_order", "by_name"},
                                        "edge_order", "edge_order = 按输入边的顺序；by_name = 按文件名排序"));
            // M_8nodes_extension L1-c（§4.7）：图片预处理 —— 与 N-02 同名参数**共用**（发送前应用）
            params.push_back(enum_param("resize_mode", "缩放模式", {"none", "fit", "fill"}, "none",
                                        "none = 不缩放；fit = 等比缩放到「最大宽/高」内；fill = 等比覆盖后居中裁剪"));
            params.push_back(int_param("resize_max_width", "最大宽", 0, 0.0, 100000.0, "0 = 不限制"));
            params.push_back(int_param("resize_max_height", "最大高", 0, 0.0, 100000.0, "0 = 不限制"));
            params.push_back(bool_param("crop_enabled", "裁剪", false, "裁剪开关（相对原图）"));
            params.push_back(int_param("crop_x", "裁剪 X", 0, 0.0, 100000.0, "裁剪起点 X"));
            params.push_back(int_param("crop_y", "裁剪 Y", 0, 0.0, 100000.0, "裁剪起点 Y"));
            params.push_back(int_param("crop_w", "裁剪宽", 0, 0.0, 100000.0, "0 = 到右侧剩余"));
            params.push_back(int_param("crop_h", "裁剪高", 0, 0.0, 100000.0, "0 = 到下侧剩余"));
            return params;
        }("official");
        registry.registerNode(std::move(definition));
    }

    // --- N-08 Text Output（输出，M4-04）-----------------------------------
    {
        Definition definition;
        definition.type         = "TextOutput";
        definition.display_name = "文本输出";
        definition.title        = "文本输出";
        definition.category     = NodeCategory::Output;
        definition.description  = "推送到 Output 窗口并透传";
        definition.inputs       = {input_port("text", "文本", PortType::Text)};
        definition.params       = {
            text_param("label", "标签", "输出", ParamType::String, false, false,
                       "最终输出的显示名（分段标题 / 文档标题）"),
            text_param("export_dir", "导出目录", std::string(), ParamType::Directory, false, false,
                       "「导出为文档…」的默认目录，也是「运行后自动导出」的目标目录；留空 = outputs/"),
            text_param("file_name", "文件名主干", std::string(), ParamType::String, false, false,
                       "留空 = 工作流名-时间戳；非法字符会被清洗"),
            bool_param("auto_export", "运行后自动导出", false,
                       "运行结束后把最终输出写入「导出目录」（不覆盖同名，自动 -1/-2）"),
            // ---- M_8nodes_extension L1-b（§4.8）：导出模板 / 批量导出 / 归档策略 ----
            text_param("export_template", "导出模板", std::string(), ParamType::Text, false, false,
                       "自定义元信息头模板（空 = 默认头）；占位符：{label} {workflow} {node_id} "
                       "{state} {duration} {time}，未匹配占位原样保留"),
            bool_param("batch_export", "批量导出", false,
                       "开启后自动导出**所有有输出的节点**到同一目录（文件名 = 主干 + 节点 id）"),
            enum_param("archive_policy", "归档策略", {"inherit"}, "inherit",
                       "inherit = 用全局 config.output.*（现状）；覆盖项字段待实施确认（L1-b 实施记录）"),
        };
        registry.registerNode(std::move(definition));
    }

    // ========================================================================
    //  M_node_expansion L2（v5 定稿 §4.3.5）：6 个文本处理节点 —— 顶层「辅助功能」分类
    //  子分类「文本处理」为伪两层（只改 node_library.cpp 渲染，见 AI_README §11）
    // ========================================================================

    // --- L2-01 TextSplit 文本分块（part2 §4.3.5.1）--------------------------
    {
        Definition definition;
        definition.type         = "TextSplit";
        definition.display_name = "文本分块";
        definition.title        = "文本分块";
        definition.category     = NodeCategory::Auxiliary;
        definition.description  = "按分隔符 / 长度 / 段落 / 正则把文本拆成多段";
        definition.inputs       = {input_port("text", "文本", PortType::Text)};
        definition.outputs      = {output_port("texts", "分块", PortType::Text)};
        definition.params       = {
            enum_param("split_mode", "拆分模式",
                       {"by_separator", "by_length", "by_paragraph", "by_regex"}, "by_separator",
                       "by_separator = 按分隔符；by_length = 按最大长度；by_paragraph = 按段落；by_regex = 按正则"),
            text_param("separator", "分隔符", "\n\n", ParamType::String, false, false,
                       "拆分分隔符（by_separator 模式；\\n = 换行）"),
            int_param("max_length", "最大长度", 2000, 1, 100000, "每块最大长度（by_length 模式）"),
            int_param("overlap", "重叠字符数", 0, 0, 100000,
                      "相邻块重叠字符数（运行时钳制到 max_length/2）"),
            bool_param("keep_separator", "保留分隔符", false,
                       "拆分时把分隔符保留在块尾（by_separator / by_paragraph / by_regex）"),
            text_param("regex_pattern", "正则", std::string(), ParamType::String, false, false,
                       "正则（by_regex 模式；匹配处作为分隔位置）"),
        };
        registry.registerNode(std::move(definition));
    }

    // --- L2-02 TextExtract 文本提取（part2 §4.3.5.2）------------------------
    {
        Definition definition;
        definition.type         = "TextExtract";
        definition.display_name = "文本提取";
        definition.title        = "文本提取";
        definition.category     = NodeCategory::Auxiliary;
        definition.description  = "按正则 / 起止标记 / JSON 路径 / 键值从文本中提取片段";
        definition.inputs       = {input_port("text", "文本", PortType::Text)};
        definition.outputs      = {output_port("texts", "多段结果", PortType::Text),
                                   output_port("text", "单段结果", PortType::Text)};
        definition.params       = {
            enum_param("extract_mode", "提取模式", {"regex", "between", "json_path", "key_value"},
                       "regex",
                       "regex = 正则匹配；between = 起止标记之间；json_path = JSON 路径；key_value = 键值提取"),
            text_param("pattern", "正则", std::string(), ParamType::String, false, false,
                       "正则（regex 模式；捕获组优先）"),
            text_param("start_marker", "起始标记", std::string(), ParamType::String, false, false,
                       "起始标记（between 模式）"),
            text_param("end_marker", "结束标记", std::string(), ParamType::String, false, false,
                       "结束标记（between 模式）"),
            text_param("json_path", "JSON 路径", std::string(), ParamType::String, false, false,
                       "点分 JSON 路径（json_path 模式，如 a.b[0].c）"),
            text_param("key_name", "键名", std::string(), ParamType::String, false, false,
                       "键名（key_value 模式；按「键: 值」行提取）"),
            enum_param("output_mode", "输出形态", {"texts", "text"}, "texts",
                       "texts = 多段（texts 端口）；text = 单段（多匹配时 text 端口输出 JSON 数组）"),
        };
        registry.registerNode(std::move(definition));
    }

    // --- L2-03 TextClean 文本清理（part2 §4.3.5.3）--------------------------
    {
        Definition definition;
        definition.type         = "TextClean";
        definition.display_name = "文本清理";
        definition.title        = "文本清理";
        definition.category     = NodeCategory::Auxiliary;
        definition.description  = "去空白 / HTML / 控制字符 / 重复行 / 前缀后缀 / AI 客套前缀";
        definition.inputs       = {input_port("text", "文本", PortType::Text)};
        definition.outputs      = {output_port("text", "文本", PortType::Text)};
        definition.params       = {
            bool_param("trim", "去首尾空白", true, "去除开头与结尾的空白"),
            bool_param("collapse_whitespace", "压缩连续空白", false,
                       "把连续空白（含换行）压缩为一个空格"),
            bool_param("remove_html", "去 HTML 标签", false, "移除 <...> 标签与常见 HTML 实体"),
            bool_param("remove_control_chars", "去控制字符", true,
                       "移除 \\x00-\\x08 / \\x0B-\\x1F 等控制字符（保留 \\n \\r \\t）"),
            bool_param("dedup_lines", "去重复行", false, "保序去重完全相同的行"),
            bool_param("remove_empty_lines", "去空行", false, "移除空白行"),
            text_param("remove_prefix", "去固定前缀", std::string(), ParamType::String, false, false,
                       "移除开头出现一次的前缀"),
            text_param("remove_suffix", "去固定后缀", std::string(), ParamType::String, false, false,
                       "移除结尾出现一次的后缀"),
            bool_param("remove_ai_prefix", "去 AI 客套前缀", false,
                       "一键移除「好的，以下是…」等 AI 客套前缀"),
        };
        registry.registerNode(std::move(definition));
    }

    // --- L2-04 TextReplace 文本替换（part2 §4.3.5.4）------------------------
    {
        Definition definition;
        definition.type         = "TextReplace";
        definition.display_name = "文本替换";
        definition.title        = "文本替换";
        definition.category     = NodeCategory::Auxiliary;
        definition.description  = "按普通文本或正则替换，支持大小写 / 全词 / 次数限制";
        definition.inputs       = {input_port("text", "文本", PortType::Text)};
        definition.outputs      = {output_port("text", "文本", PortType::Text)};
        definition.params       = {
            text_param("pattern", "查找模式", std::string(), ParamType::String, false, false,
                       "要查找的文本或正则（空 = 不做替换）"),
            text_param("replacement", "替换内容", std::string(), ParamType::String, false, false,
                       "替换为（空 = 删除匹配）"),
            bool_param("use_regex", "正则模式", false, "把「查找模式」当作正则"),
            bool_param("case_sensitive", "大小写敏感", true, "false = 忽略大小写"),
            int_param("max_replacements", "最大替换次数", 0, 0, 1000000, "0 = 不限次数"),
            bool_param("whole_word", "全词匹配", false, "仅匹配整词（中英文均按词边界判定）"),
        };
        registry.registerNode(std::move(definition));
    }

    // --- L2-05 TextStats 文本统计（part2 §4.3.5.5）--------------------------
    {
        Definition definition;
        definition.type         = "TextStats";
        definition.display_name = "文本统计";
        definition.title        = "文本统计";
        definition.category     = NodeCategory::Auxiliary;
        definition.description  = "字符 / 中文字数 / 英文词数 / 行 / 段落 / 词频统计";
        definition.inputs       = {input_port("text", "文本", PortType::Text)};
        definition.outputs      = {output_port("text", "统计文本", PortType::Text),
                                   output_port("number", "中文字数", PortType::Number)};
        definition.params       = {
            enum_param("stats_mode", "统计模式", {"basic", "word_freq", "all"}, "basic",
                       "basic = 基础统计；word_freq = 词频 Top N；all = 两者"),
            int_param("top_n", "词频 Top N", 10, 1, 100, "词频统计输出条数"),
            enum_param("output_mode", "输出形态", {"text", "number"}, "text",
                       "number = 只输出中文字数到 number 端口；text = 输出统计文本到 text 端口"),
        };
        registry.registerNode(std::move(definition));
    }

    // --- L2-06 TextDiff 文本对比（part2 §4.3.5.6）---------------------------
    {
        Definition definition;
        definition.type         = "TextDiff";
        definition.display_name = "文本对比";
        definition.title        = "文本对比";
        definition.category     = NodeCategory::Auxiliary;
        definition.description  = "对比两段文本差异（自写简单 LCS；line / word / char 粒度）";
        definition.inputs       = {input_port("text_a", "文本 A", PortType::Text),
                                   input_port("text_b", "文本 B", PortType::Text)};
        definition.outputs      = {output_port("text", "差异文本", PortType::Text)};
        definition.params       = {
            enum_param("diff_mode", "对比粒度", {"line", "word", "char"}, "line",
                       "line = 按行；word = 按词；char = 按字符"),
            int_param("context_lines", "上下文行数", 3, 0, 20, "差异块上下保留的未变化行数"),
            enum_param("output_mode", "输出形态", {"text", "json"}, "text",
                       "text = 可读差异文本；json = 操作序列 JSON"),
        };
        registry.registerNode(std::move(definition));
    }
}

// ============================================================================
//  PB2-24 要点②：`provider` 动态枚举（实现）
// ============================================================================

std::vector<std::string> provider_option_ids()
{
    const ai::ProviderSpecs& specs        = ai::provider_specs();
    std::vector<std::string>  official_ids = specs.ids("official");
    std::vector<std::string>  web_ids      = specs.ids("web");
    std::vector<std::string>  ids;
    ids.reserve(official_ids.size() + web_ids.size());
    ids.insert(ids.end(), official_ids.begin(), official_ids.end());
    ids.insert(ids.end(), web_ids.begin(), web_ids.end());
    return ids;
}

namespace {

// 就地替换 Definition 里 `provider` 的枚举（默认值不在新枚举里 ⇒ 落到首项）
void apply_provider_options(Definition& definition, const std::vector<std::string>& ids)
{
    for (Param& param : definition.params) {
        if (param.id != "provider" || param.type != ParamType::Enum) {
            continue;
        }
        param.enum_options = ids;
        const std::string current =
            param.default_value.is_string() ? param.default_value.get<std::string>() : std::string();
        if (!ids.empty() && std::find(ids.begin(), ids.end(), current) == ids.end()) {
            param.default_value = ids.front();
        }
        return;
    }
}

// 就地替换节点实例里 `provider` 的枚举（**保留当前值**：可能是刚新建的站点 id）
void apply_provider_options(Node& node, const std::vector<std::string>& ids)
{
    for (Param& param : node.params) {
        if (param.id != "provider" || param.type != ParamType::Enum) {
            continue;
        }
        param.enum_options = ids;
        if (param.is_empty() && !ids.empty()) {
            param.value = ids.front();
        }
        return;
    }
}

} // namespace

void refresh_provider_options()
{
    const std::vector<std::string> ids = provider_option_ids();
    if (ids.empty()) {
        return; // 表不可用（极小概率）：保留原枚举，绝不把下拉清空
    }
    if (Definition* definition = NodeRegistry::instance().findMutable("ProviderConfig")) {
        apply_provider_options(*definition, ids);
    }
}

void refresh_provider_options(Graph& graph)
{
    const std::vector<std::string> ids = provider_option_ids();
    if (ids.empty()) {
        return;
    }
    if (Definition* definition = NodeRegistry::instance().findMutable("ProviderConfig")) {
        apply_provider_options(*definition, ids);
    }
    for (Node& node : graph.nodes) {
        apply_provider_options(node, ids);
    }
}

void reload_provider_specs_and_refresh(Graph* graph)
{
    ai::reload_provider_specs();
    if (graph != nullptr) {
        refresh_provider_options(*graph);
    }
    else {
        refresh_provider_options();
    }
}

std::vector<std::string> provider_param_options()
{
    const Definition* definition = NodeRegistry::instance().find("ProviderConfig");
    if (definition == nullptr) {
        return {};
    }
    for (const Param& param : definition->params) {
        if (param.id == "provider" && param.type == ParamType::Enum) {
            return param.enum_options;
        }
    }
    return {};
}

} // namespace aiwrite::engine
