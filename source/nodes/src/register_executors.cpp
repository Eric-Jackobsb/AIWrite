#include "nodes/nodes.h"

#include "engine/executor.h"

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
}

} // namespace aiwrite::nodes
