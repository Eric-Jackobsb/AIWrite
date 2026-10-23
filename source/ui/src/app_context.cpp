#include "app_context.h"

namespace aiwrite::ui {

// ARCH-A3：构造顺序 = §1.8.11（成员声明顺序）：
//   1. EventBus（WorkflowModel 成员）→ 2. Config → 3. Clipboard → 4. Diagnostics →
//   5. WorkflowModel → 6. SelectionState → 7. RunSession → 8. AutosaveService（依赖 model）→
//   9. WorkflowFile（依赖 model）→ 编排层 editor（注入上列 8 个引用，不拥有任何数据）。
// 初始化列表按声明顺序写（避免 -Wreorder）；未列出的成员走默认构造。
AppContext::AppContext()
    : autosave(model),
      workflow_file(model),
      editor(model, selection, run, config, clipboard, diagnostics, autosave, workflow_file) {}

} // namespace aiwrite::ui
