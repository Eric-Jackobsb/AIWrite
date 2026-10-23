#include "app_context.h"

namespace aiwrite::ui {

// 成员声明顺序 = 构造顺序：config / clipboard / diagnostics（无依赖）→
// autosave / workflow_file（依赖 model；经 model.event_bus() 挂订阅）
AppContext::AppContext(WorkflowModel& model) : autosave(model), workflow_file(model) {}

} // namespace aiwrite::ui
