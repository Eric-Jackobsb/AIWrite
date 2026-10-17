#pragma once

#include <string>

namespace aiwrite::ui {

// 主程序启动参数
struct AppOptions {
    bool console = false; // 为日志额外分配控制台窗口（便于开发调试）
    // 启动即打开的工作流文件（CLI：--open <路径>；空 = 用内置示例工作流）
    //  * 用途①：用户从命令行 / 文件关联直接打开某个工作流
    //  * 用途②：把「打开某个大图 → 看 FPS」的复现步骤固定下来（如
    //            `aiwrite.exe --open source\workflows\testAllAI.json`），
    //            配合 app.log 里的 `[诊断] 画布` 行可离线复核帧率与纹理缓存命中率
    std::string open_workflow;
    // [调试] --auto-open-composite：启动第 8 帧程序化走「新建上级节点」路径
    //（落盘空白规格 JSON → composite_editor_open），配合窗口栈看门狗复现/定位
    // 「Mismatched Begin/BeginChild vs End/EndChild」断言。
    bool auto_open_composite = false;
};

int run(const AppOptions& options);

} // namespace aiwrite::ui
