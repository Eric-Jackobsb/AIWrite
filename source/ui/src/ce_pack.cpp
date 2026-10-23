#include "composite_editor.h"
#include "ce_internal.h"
#include "composite.h"
#include "editor_controller.h"

#include <string>

namespace aiwrite::ui {

// ==================================================== 打包选区为上级节点 ====
// ARCH-E6：打包下沉 —— 规格构建（内部图 / 输入 / 输出 / 暴露参数）已迁入
// engine::buildCompositeSpecFromSelection（打包-校验同源，§4 验收判据 4）；
// 生成后严格校验：先 validate_composite_spec 再 save（产物必然合法规格）。
std::string compositePackSelection(const std::string& name, std::string* error,
                                   EditorController& editor)
{
    EditorController& state = editor;

    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return std::string();
    };

    engine::CompositeSpec spec;
    std::string build_error;
    if (!engine::buildCompositeSpecFromSelection(state.model().graph, state.selection().nodes(),
                                                 name, &spec, &build_error)) {
        return fail(build_error);
    }

    // ---- 生成后严格校验：打包产物必须通过完整规格校验（打包-校验同源）----
    std::string validate_error;
    if (!engine::validate_composite_spec(spec, &validate_error)) {
        return fail("打包生成的规格未通过校验：" + validate_error);
    }

    spec.id = engine::CompositeRegistry::instance().makeId();
    std::string save_error;
    if (!engine::CompositeRegistry::instance().save(spec, &save_error)) {
        return fail("打包失败：" + save_error);
    }
    compositeEditorOpen(spec.id);
    if (error != nullptr) {
        error->clear();
    }
    return spec.id;
}

} // namespace aiwrite::ui
