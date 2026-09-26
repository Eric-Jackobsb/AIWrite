#include "engine/node_registry.h"

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
        definition.description  = "输出图片路径，执行时校验文件存在";
        definition.outputs      = {output_port("image", "图片", PortType::Image)};
        definition.params       = {file_param("path", "图片路径", /*required*/ true,
                                              "支持 png / jpg / jpeg / bmp / webp")};
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
        definition.params       = {text_param("template", "模板", "请根据以下内容续写：\n{vars}",
                                              ParamType::Text, /*required*/ true, false,
                                              "用 {端口名} 引用输入值；变长输入按顺序可用 {1}/{2}…（单值时也可写 {vars}）")};
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
                                   bool_param("ignore_empty", "忽略空输入", true)};
        registry.registerNode(std::move(definition));
    }

    // --- N-05 Provider Config（配置）---------------------------------------
    {
        Definition definition;
        definition.type         = "ProviderConfig";
        definition.display_name = "提供商配置";
        definition.title        = "提供商配置";
        definition.category     = NodeCategory::Config;
        definition.description  = "输出 provider 句柄，校验配置完整性";
        definition.outputs      = {output_port("provider", "提供商", PortType::Provider)};
        definition.params       = {
            enum_param("provider", "提供商", {"deepseek"}, "deepseek"),
            enum_param("mode", "模式", {"official", "web"}, "official",
                       "official = 官方 API Key；web = 网页版登录（M4 实现）"),
            text_param("api_base", "API 地址", "https://api.deepseek.com", ParamType::String),
            enum_param("model", "模型", {"deepseek-chat", "deepseek-reasoner"}, "deepseek-chat"),
            // M5-02：模型名解锁 —— 留空用上方枚举；填第三方/本地模型名则覆盖
            // （DeepSeek 官方 API 无视觉模型，图片理解需指向兼容 VLM，如智谱 glm-4v-flash）
            text_param("model_custom", "模型（自定义）", std::string(), ParamType::String, false, false,
                       "留空 = 用上方「模型」；填写则覆盖（例：glm-4v-flash / Qwen/Qwen2.5-VL-72B-Instruct）"),
            text_param("api_key", "API Key", std::string(), ParamType::String,
                       /*required*/ false, /*secret*/ true,
                       "仅保存在内存中；M4-07 起改用 Windows Credential Manager"),
            text_param("api_key_ref", "Key 引用名", "brain-ai/deepseek", ParamType::String,
                       false, false, "凭据管理器中的条目名（M4-07）"),
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
        definition.params       = {
            text_param("system_prompt", "系统提示词", std::string(), ParamType::Text),
            enum_param("mode", "模式", {"official", "web"}, "web",
                       "web = 网页版（已接线，需先登录一次）；official = 官方 API（已接线，需 API Key）"),
            // 注意：max 必须 ≤ IM_S32_MAX/2（1073741823），否则参数面板 SliderInt 断言崩溃
            int_param("seed", "随机种子", 0, 0.0, 1000000000.0,
                      "0 = 不指定；点「重新生成（新 seed）」会写入新值（官方 API 随请求发送，网页版忽略）"),
            enum_param("model", "模型",
                       {"deepseek-chat", "deepseek-reasoner", "expert"}, "deepseek-chat",
                       "deepseek-reasoner 走网页版「深度思考」；expert 走专家模式"),
            float_param("temperature", "温度", 0.7, 0.0, 2.0, 0.1, "采样温度，越高越随机"),
            int_param("max_tokens", "最大长度", 2048, 1.0, 8192.0, "生成的最大 token 数"),
            float_param("top_p", "Top P", 1.0, 0.0, 1.0, 0.05),
            bool_param("stream", "流式输出", true, "M4 实现流式回调"),
        };
        registry.registerNode(std::move(definition));
    }

    // --- N-07 VLM Generate（推理，M5-02）----------------------------------
    {
        Definition definition;
        definition.type         = "VLMGenerate";
        definition.display_name = "图片理解";
        definition.title        = "图片理解";
        definition.category     = NodeCategory::Inference;
        definition.description  = "读取 prompt + 图片 + provider，由视觉模型生成文字";
        definition.inputs       = {input_port("prompt", "提示词", PortType::Text),
                                   input_port("image", "图片", PortType::Image),
                                   input_port("provider", "提供商", PortType::Provider,
                                              false, /*optional*/ true)};
        definition.outputs      = {output_port("text", "文本", PortType::Text)};
        definition.params       = {
            text_param("system_prompt", "系统提示词", std::string(), ParamType::Text),
            float_param("temperature", "温度", 0.7, 0.0, 2.0, 0.1),
            int_param("max_tokens", "最大长度", 2048, 1.0, 8192.0),
            float_param("top_p", "Top P", 1.0, 0.0, 1.0, 0.05),
            bool_param("stream", "流式输出", true),
        };
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
        };
        registry.registerNode(std::move(definition));
    }

    // --- N-09 Image Preview（输出，M5-03）---------------------------------
    {
        Definition definition;
        definition.type         = "ImagePreview";
        definition.display_name = "图片预览";
        definition.title        = "图片预览";
        definition.category     = NodeCategory::Output;
        definition.description  = "推送到 Output 窗口并透传";
        definition.inputs       = {input_port("image", "图片", PortType::Image)};
        definition.params       = {text_param("label", "标签", "图片", ParamType::String)};
        registry.registerNode(std::move(definition));
    }
}

} // namespace aiwrite::engine
