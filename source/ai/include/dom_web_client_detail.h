#pragma once

// ============================================================================
//  通用 DOM 站点适配器 —— **模块内契约**(ARCH-Z1b)
//
//  * 只允许 `ai/src/dom_web_client*.cpp` 自己 include；**不是**公开 API
//  * 公开语义入口 = `dom_web_client.h`（本文件只把「原本一个 TU 内的私有件」
//    显式化成 4 个 TU 之间的契约）
//  * 拆分版图（逐字搬迁，零语义变化）：
//      `dom_web_client.cpp`            会话聊天主体：clamp/cfg/脚本渲染/formatAnswerText/domChat
//      `dom_web_client_probe.cpp`      单站探测：selector dump + 探测脚本执行 + 结果解析
//      `dom_web_client_probe_all.cpp`  批量探测：domProbeAll（报告 + 台账）
//      `dom_web_client_selftest.cpp`   自检：domAdapterSelftest + domProbeSelftest
//      `dom_web_client_scripts.h`      配置常量 + kickoff/prepare/send-verify/click 脚本模板
//      `dom_web_client_scripts_poll.h` poll/probe/discover 脚本模板
// ============================================================================

#include "dom_web_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "log.h"
#include "upload_registry.h" // M8-14：站点上传模板（图片先上传再发送）
#include "webview_host.h"

namespace aiwrite::ai {

// ---- 探测辅助（原匿名名字空间；`dom_web_client_probe.cpp` 定义）----
//  * 批量探测 / 自检两个 TU 复用同一口径（状态文案 / 判据文案 / 报告块 / 时间戳）
std::string probe_timestamp();
std::string probe_state_of(const DomSiteProbe& p);
std::string probe_state_text(const DomSiteProbe& p);
std::string probe_criteria_text(const DomSiteProbe& p);
void        print_probe_block(int index, int total, const DomSiteProbe& p);

} // namespace aiwrite::ai
