#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// 配置管理（字段与设计文档 20.2 节 config.toml 示例一致）
namespace aiwrite {

struct Config {
    int config_version = 1;

    // 界面语言 / 启动行为（原 `General`：`language` / `startup`）—— `PQ-18②`（2026-10-09）**已移除**：
    //  * 决策 = i18n **不做**（见 roadmap.md §十），`config.toml` 不再有 `[general]` 节；归口 **M6** 再议
    //  * 旧文件里的 `[general]` 被**静默忽略**（`read_value` 只读已知键），下次保存不再写回

    struct Ui {
        // P7a-12（UI A 档）：**新用户首屏即可见**节点库 + 参数面板（旧默认 false → true）
        //  * 老配置兼容（P7a-13）：`read_value` 只在键**存在**时覆盖 → 显式写了 false 的用户保持 false
        bool show_node_library   = true;
        bool show_property_panel = true;
        bool show_console        = true;
        bool show_output_window  = false;
        int  console_height      = 120;
        bool show_grid           = true; // 画布网格显隐（默认开；2026-10-15 确认保留，要删的是节点上的灰色分隔线）
        int  grid_size           = 20;
        bool running_animation   = true;

        // ---- 窗口几何（F3 / PD-04）：主窗口尺寸 / 位置 / 最大化 ----
        //  * window_width / height ≤ 0 → 用内置默认（按工作区 90% 且不超过 1600x1000）
        //  * window_pos_x / pos_y < 0 → 未记录位置（启动时不设置位置，交给系统/居中）
        //  * 启动读取时经 utils::fit_window_to_workarea 越屏矫正后再应用
        int  window_width     = 0;
        int  window_height    = 0;
        int  window_pos_x     = -1;
        int  window_pos_y     = -1;
        bool window_maximized = false;

        // ---- 首启引导（M6 `PM-02`）：是否已展示过「首次使用引导」----
        //  * 由引导窗口的「开始使用 / 跳过」写入；**不**影响任何功能（只是不再自动弹）
        //  * 老配置无此键 → 默认 false ⇒ **升级后的老用户会看到一次引导**（可跳过，无副作用）；
        //    如需再次查看：菜单「帮助 → 首次使用引导」
        bool first_run_done = false;

        // ---- 自动保存与崩溃恢复（M_patchAB §4 · PC-01 / PC-02）----
        //  * `autosave_enabled`：总开关（默认开）
        //  * `autosave_idle_sec`：变更后**静默**多少秒才写快照（默认 3；≥1）
        //  * `autosave_keep`：历史快照份数 `autosave_1..N`（默认 3 = 与 `kAutosaveKeepDefault`
        //    一致；0 = 不留历史）。**落点**：`~/.brain-ai/snapshots/autosave.json` + `_<N>.json`
        //  * `autosave_restore_prompt`：启动时发现快照比工作流新 ⇒ 是否弹「恢复未保存的更改？」
        //    （默认 true；窗口里的「不再提示」把它写成 false —— PC-02 的「可禁用该提示」）
        //  * 老配置无这些键 → 全部取默认值（行为 = 「自动保存开、3 秒、留 3 份、会提示」）
        bool autosave_enabled        = true;
        int  autosave_idle_sec       = 3;
        int  autosave_keep           = 3;
        bool autosave_restore_prompt = true;
    };

    struct Output {
        std::string archive_dir = "~/.brain-ai/outputs";
        int  ttl_days           = 30;
        bool auto_open_on_complete = false;   // 仍留 M5（Patch C 未接）
        bool keep_history       = false;
        int  max_history        = 10;
    };   // 说明（PC-05/06 已接线）：运行结束自动归档「每节点 .txt + run.json」到 archive_dir；
         //      keep_history=false → 仍保留最近 kHistoryFloor 份（默认 3，PW-11），true → 保留 max_history 份；
         //      ttl_days>0 → 清理超期目录

    struct Timeout {
        int connect_ms     = 10000;
        int first_byte_ms  = 30000;
        int stream_idle_ms = 300000;
    };

    struct Error {
        bool retry_enabled     = true;
        int  retry_count       = 1;
        int  retry_interval_ms = 2000;
    };

    struct Advanced {
        std::string config_path = "~/.brain-ai/config.toml";
        std::string log_dir     = "~/.brain-ai/logs";
        int  log_ttl_days       = 30;
    };

    // ---- M_patchAB §2.3（`PB-06` 剩余）：**网络（代理 / 自签证书）** ----
    //  * `proxy`：HTTP(S) 代理地址（例 `http://127.0.0.1:7890`）；**空 ⇒ 不显式代理**
    //  * `proxy_from_env`：为真时**读环境变量**（`HTTPS_PROXY` / `HTTP_PROXY` / `ALL_PROXY`，
    //    按目标 scheme 选；`NO_PROXY` 命中 ⇒ 该项**直达**不代理）
    //    —— 两者同时存在时 **`proxy` 显式值优先**（企业网络里手填一次即可，避免环境串味）
    //  * `no_proxy`：显式排除清单（逗号分隔；`*` = 全部排除）；空 ⇒ 只读环境变量 `NO_PROXY`
    //  * `insecure_tls`：**自签证书开关**，默认 **false = 严格校验**（打开会让站点证书不再被校验，
    //    仅用于企业内网自签证书；界面按**危险项**红字提示）
    struct Network {
        std::string proxy;
        bool        proxy_from_env = true;
        std::string no_proxy;
        bool        insecure_tls   = false;
    };

    // ---- M_patchAB §2.3（`PB-06` 剩余）：**凭据库**（`[credentials]`）----
    //  * 此前这三项**写死在代码里**（`~/.brain-ai/credentials` / TTL 30 天 / backend `dpapi`）
    //  * `backend`：本版本**只支持** `dpapi`（Windows DPAPI，当前用户作用域）；其它值 ⇒ 明确报错
    //  * `ttl_days`：凭据「即将过期」提示与 `purge_expired_credentials()` 的阈值（0 = 不清理）
    //  * `dir`：空 ⇒ 用默认目录（`~/.brain-ai/credentials`）
    struct Credentials {
        std::string backend  = "dpapi";
        std::string dir      = "~/.brain-ai/credentials";
        int         ttl_days = 30;
    };

    struct Provider {
        std::string provider    = "deepseek";
        std::string mode        = "official";
        std::string api_base    = "https://api.deepseek.com";
        std::string model       = "deepseek-chat";
        std::string api_key_ref = "brain-ai/deepseek";
    };

    Ui       ui;
    Output   output;
    Timeout  timeout;
    Error    error;
    Advanced advanced;
    // M_patchAB §2.3（`PB-06` 剩余）：网络（代理 / 自签证书）与凭据库（`[network]` / `[credentials]`）
    Network     network;
    Credentials credentials;
    // 旧成员（兼容）：与 `providers["deepseek"]` 互为镜像（加载时同步；保存时作为 deepseek 条的权威）
    Provider deepseek;

    // M_patchB L1（PB2-06）：多 provider **实例参数**（配置表条目 id → 实例参数）
    //  * `config.toml` 的 `[providers.<id>]` 节；**厂商元数据**仍以配置表（`assets/providers.json`
    //    + 用户覆盖层）为准，这里只存「用哪个 provider / 地址覆盖 / 默认模型 / 引用名」
    //  * 旧文件只有 `[providers.deepseek]` 一节 → 加载时自动迁移为映射里的一条（幂等）
    //  * 保存前自动备份 `config.toml.bak`（写失败不覆盖原文件）
    std::map<std::string, Provider> providers;
    // ---- M_patchAB §4（`PC-04` / `DBG-M6-02`）：版本迁移 ----
    //  * 由 `engine::migrate_config()` 在**加载后**设置：true = 磁盘上的 `config.toml`
    //    版本比本程序新 ⇒ `save_config()` **拒绝覆盖写回**（避免把新版配置降级写坏，`I21`）
    bool version_newer_than_supported = false;
};

// 读取配置；文件不存在时生成默认配置并落盘。返回是否成功。
bool load_config(const std::filesystem::path& file, Config& out);

// ---- M_patchAB §4（`PC-04` / `DBG-M6-02`）：配置文件**版本迁移**（纯逻辑，可离线断言）----
//  * 版本缺失（老文件）⇒ 补为当前支持版本（`migrated=true` + 说明）
//  * 版本比程序新 ⇒ `future=true` 且置 `Config::version_newer_than_supported`
//    ⇒ `save_config()` **拒绝覆盖写回**（不把新版配置降级写坏）
struct ConfigMigrationOutcome {
    bool                     ok       = true;
    bool                     migrated = false;
    int                      from     = 0; // 0 = 文件里没有版本号（老文件）
    int                      to       = 1;
    bool                     future   = false;
    std::vector<std::string> notes;
    std::string              error;
};

ConfigMigrationOutcome migrate_config(Config& config);
// 当前支持的配置版本（`Config::config_version` 上限的单一来源）
int                     current_config_version();

// 写入配置（自动创建父目录）
bool save_config(const std::filesystem::path& file, const Config& config);

// 进程级配置缓存（PA-07：地基补丁 A）
//  * 启动时由 app / api_probe 以 set_app_config(load_config(...)) 灌入一次
//  * 供非 UI 代码（provider / 工具 / 自检）读取；未设置时返回默认构造值
//  * 只读约定：返回引用；set_app_config 仅发生在启动/显式保存时，不要在读取期间并发写入
const Config& app_config();
void          set_app_config(Config config);

// PA-08：**尚未接线**的配置字段清单（消除"改了 config 却没效果"的困惑）
//  * 已接线：ui.show_grid / ui.console_height / ui.running_animation /
//    ui.show_node_library / ui.show_property_panel / ui.show_console / ui.show_output_window /
//    ui.window_*（F3 窗口几何）/ output.archive_dir / output.keep_history / output.max_history /
//    output.ttl_days（PC-05 归档）/ ui.autosave_*（PC-01/02 自动保存与恢复）/
//    timeout.* / error.*（M_patchAB 批 4：官方 API 的默认超时与重试）/
//    **network.* / credentials.***（M_patchAB 批 7 · `PB-06` 剩余：代理 / 自签证书 / 凭据库三参数）
//  * ui.grid_size：设计上属"网格间距"，但 vendored imgui-node-editor 的 Style 没有该字段，
//    当前无法生效（如实登记，归口 PD-03 处理：升级库或自绘背景网格）
//  * 已移除（`PQ-18②` · 2026-10-09）：general.language / general.startup —— 界面语言 / 启动行为不做
//  * 节点参数为准：providers.deepseek.*（仅作默认值来源，实际以 ProviderConfig 节点参数为准）
//  * 其余字段在加载时写 `[配置] 以下字段尚未生效：…` 日志，并在使用说明中标注归口补丁
std::vector<std::string> unwired_config_fields();

} // namespace aiwrite
