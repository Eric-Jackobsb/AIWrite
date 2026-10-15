#include "nodes.h"

#include "executor.h"

namespace aiwrite::nodes {

// 注册全部节点执行函数（幂等：同名覆盖）
void registerAllExecutors()
{
    engine::NodeExecutorRegistry& registry = engine::NodeExecutorRegistry::instance();

    registry.registerExecutor("TextInput", &execute_text_input);
    registry.registerExecutor("ImageInput", &execute_image_input);
    registry.registerExecutor("PromptTemplate", &execute_prompt_template);
    registry.registerExecutor("TextMerge", &execute_text_merge);
    registry.registerExecutor("ProviderConfig", &execute_provider_config);
    registry.registerExecutor("LLMGenerate", &execute_llm_generate);
    registry.registerExecutor("VLMGenerate", &execute_vlm_generate);
    registry.registerExecutor("TextOutput", &execute_text_output);
    // M_node_expansion L2（v5 定稿 §4.3.5）：6 个文本处理节点
    registry.registerExecutor("TextSplit", &execute_text_split);
    registry.registerExecutor("TextExtract", &execute_text_extract);
    registry.registerExecutor("TextClean", &execute_text_clean);
    registry.registerExecutor("TextReplace", &execute_text_replace);
    registry.registerExecutor("TextStats", &execute_text_stats);
    registry.registerExecutor("TextDiff", &execute_text_diff);
}

} // namespace aiwrite::nodes
