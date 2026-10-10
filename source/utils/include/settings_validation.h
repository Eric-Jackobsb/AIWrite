#pragma once

// ============================================================================
//  配置校验（M6 `M6-01` / `PD-03` 设置面板的**纯函数**内核）
//
//  * 为什么放在 `utils/`：设置面板在 GUI 目标（`ui/settings_panel.cpp`），而校验逻辑
//    必须能被**离线断言**（`api_probe --settings-assert`）⇒ 放在核心库 utils 里，
//    不依赖 ImGui / 窗口 / 磁盘
//  * 只做**范围与必填**检查；不做「路径是否存在」（用户可以先填后建目录）
// ============================================================================

#include <string>

#include "config.h"

namespace aiwrite::utils {

// 返回**首个**错误的中文说明（面向非技术用户：说清「哪一项、该填多少、当前多少」）
//  * 空串 = 通过
std::string validate_settings(const Config& config);

} // namespace aiwrite::utils
