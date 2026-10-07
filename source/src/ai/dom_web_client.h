#pragma once

// ============================================================================
//  通用网页版适配器（DOM 驱动）—— M_patchB L3（PB2-13）
//
//  设计目标：把「任意站点」降级为**纯数据**（配置表 `web` 段的选择器 + 轮询参数），
//  程序里不出现任何站点专有常量。
//
//  执行链（全部在**已登录窗口**的页面上下文里完成；技术基础 = webview_host 的 ExecuteScript）：
//    1) 按站点 ensure_session（凭证按 origin 隔离；未登录 → 给可操作指引，不代填密码、不绕过验证）
//    2) 注入提示词：contenteditable → insertText（触发 input 事件）；input/textarea → 原生 setter + input/change
//    3) 触发发送：send.kind = key（默认 Enter → keydown/keypress/keyup）/ click（点击 send_value 选择器）
//    4) 轮询 answer_selector 的 innerText（**最后一个**匹配节点 = 本轮回答）：
//         · done_when = selector_present / selector_gone → 条件成立即完成
//         · 未配 done_when → 「文本连续 stable_rounds 轮不变」视为完成
//         · 到达上限 → **如实返回已取文本 + 明确警告**（R13：不假装成功、不无限等待）
//
//  M8-37（零开关）：2)/3)/4) 的**选择器缺失不再拦路** —— 声明值未命中时运行期自动识别：
//    · 输入框：候选池打分（可见 + 靠视口下半部 + 有 placeholder）
//    · 发送：click 未命中时自动找可见发送按钮（key 默认 Enter）
//    · 答案容器：**发送前基线快照** → 取「新出现 / 文本增长」且足够长的**文档顺序最后**候选
//      （不猜类名；自动排除输入区与「自己那条回声」）；识别结果只**打印**，不回写配置（口径①）
//
//  纯函数（离线可断言，见 `VB2-22`）：clamp_poll_params / dom_cfg_json /
//  dom_kickoff_script / dom_poll_script / dom_probe_script
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "ai/provider_spec.h"
#include "web/web_owner.h" // M_patchC（PW-01）：归属（纯数据；无 Win32 依赖）

namespace aiwrite::ai {

struct DomChatRequest {
    std::string     prompt;              // 提示词
    ProviderWebSpec site;                // 站点（选择器 / done_when / 轮询参数 / 登录页）
    std::string     provider_id;         // 条目 id（日志 / 站点键归属）
    int             timeout_ms = 180000; // 总超时（含 ensure_session）
    // ---- M8-14：图片（**先上传，再发送**；不变量 `I18`）----
    //  * 值形态：`aiwrite-asset:<摘要>` 令牌 或 绝对路径（`upload_images` 内部再兜一层解析，`I19`）
    //  * 非空时：走**站点上传模板**（`ai/upload/**`）→ **双证据齐备**才注入提示词；
    //    否则**不发送**并如实报错（绝不静默降级、绝不假装成功）
    std::vector<std::string> image_values;
    std::size_t              image_max_bytes = 0; // 站点 / 表限额（0 = 用站点单元默认）
    // ---- M_patchC（`PW-01` · 不变量 `I24` / `I26`）：**归属** ----
    //  * 由节点层填（`run_id` = 本次运行 · `node_id` = 当前节点）；空 = 未标注（CLI / 自检）
    //  * 一路带到 `web/webview_host`（日志 / 证据归属 / `PW-02` 窗口表仲裁）
    web::WebOwner owner;
};

struct DomChatResult {
    bool        ok = false;              // true = 至少取到文本（可能带 warning）
    std::string text;                    // 回答正文
    std::string warning;                 // 已达上限 / 文本未稳定（R13：如实提示）
    std::string error;                   // ok=false 时的可操作原因
    int         polls = 0;               // 实际轮询次数
    long long   elapsed_ms = 0;
    std::string steps;                   // 诊断摘要（图片上传步骤 + input / send / answer 命中数）
    // ---- M8-14：图片上传阶段的诊断（进 Console / 节点输出）----
    bool                     uploaded = false; // 是否执行了上传**且双证据齐备**
    std::string              upload_page;      // 页面证据（例：命中 2 个（…））
    std::string              upload_net;       // 网络回执（例：POST 200 …）
    std::vector<std::string> conversions;      // 图片转换说明（每张一行）
    // ---- M8-37：运行期**自动识别**（零开关：选择器未回填不再拦路）----
    //  * 口径①（用户拍板）：**只打印不落盘** —— 识别结果只进 Console / steps，**不**回写任何配置
    //  * 用途：`dom_chat` 的脚本在「声明值未命中」时自动识别输入框 / 答案容器；
    //    这里把「实际用什么跑的 + 页面指纹」如实带出来，便于**可选**回填 providers.json
    bool        input_auto = false;         // true = 输入框由运行期自动识别（非声明值）
    std::string input_used;                 // 实际使用的输入框（声明值或自动识别值）
    bool        answer_auto = false;        // true = 答案容器由「基线新增节点差分」自动识别
    std::string answer_fingerprint;         // 自动识别到的答案容器指纹（例：div.markdown）
};

// 轮询参数钳制：poll_ms ∈ [200, 2000]、max_polls ∈ [10, 600]（R13：有硬上限）
void clamp_poll_params(const ProviderWebSpec& site, int* poll_ms, int* max_polls);

// 注入给页面的配置 JSON（**转义安全**：提示词含引号 / 换行也不会破坏脚本）
std::string dom_cfg_json(const DomChatRequest& request);
// 常量脚本（读 `window.__aiwriteDom`）：kickoff = 写入提示词 + 触发发送；poll = 取答案 + 判结束
std::string dom_kickoff_script();
std::string dom_poll_script();
// PB2-15：选择器探测脚本（读 `window.__aiwriteDomProbe`；返回各选择器命中数 / 可见性 / done_when 现状 / token 形状）
std::string dom_probe_script();

// 执行（需要已登录窗口；内部完成 ensure_session + 页面交互）
DomChatResult dom_chat(const DomChatRequest& request);

// PB2-15：**选择器探测 / 诊断**（只读，不发送任何内容）—— 按条目诊断 dom 站点
//  * 打印：当前页面 URL/标题、各选择器命中数、`done_when` 现状、`token_expr` 取值形状、Cookie 可读性
//  * 并给**可操作修复建议**（F12 Copy selector / 改 send 方式 / 修 token_expr）
//  * 返回码：0 = 必需选择器全部命中；1 = 有缺项；2 = 站点不可用 / 非 dom 条目
int dom_adapter_selftest(const std::string& provider_id, int timeout_ms);

// M_patchB L4（PB2-29 / v15）：**选择器候选枚举**（只读）—— 让「填选择器」不必靠人肉 F12
//  * 在当前页面枚举候选 input / textarea / contenteditable、发送按钮、回答容器，打印指纹 + **建议选择器**
//  * 返回码：0 = 枚举成功；1 = 页面 / 脚本未就绪；2 = 条目不可用（非网页版条目 / 缺 login_url / 表外 id）
int dom_selector_dump(const std::string& provider_id, int timeout_ms);

} // namespace aiwrite::ai
