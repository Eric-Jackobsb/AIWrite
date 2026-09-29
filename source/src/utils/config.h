#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// 配置管理（字段与设计文档 20.2 节 config.toml 示例一致）
namespace aiwrite {

struct Config {
    int config_version = 1;

    struct General {
        std::string language = "zh-CN";
        std::string startup  = "welcome";
    };

    struct Ui {
        // P7a-12（UI A 档）：**新用户首屏即可见**节点库 + 参数面板（旧默认 false → true）
        //  * 老配置兼容（P7a-13）：`read_value` 只在键**存在**时覆盖 → 显式写了 false 的用户保持 false
        bool show_node_library   = true;
        bool show_property_panel = true;
        bool show_console        = true;
        bool show_output_window  = false;
        int  console_height      = 120;
        bool show_grid           = true;
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
    };

    struct Output {
        std::string archive_dir = "~/.brain-ai/outputs";
        int  ttl_days           = 30;
        bool auto_open_on_complete = false;   // 仍留 M5（Patch C 未接）
        bool keep_history       = false;
        int  max_history        = 10;
    };   // 说明（PC-05/06 已接线）：运行结束自动归档「每节点 .txt + run.json」到 archive_dir；
         //      keep_history=false → 只留最近 1 份，true → 保留 max_history 份；ttl_days>0 → 清理超期目录

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

    struct Provider {
        std::string provider    = "deepseek";
        std::string mode        = "official";
        std::string api_base    = "https://api.deepseek.com";
        std::string model       = "deepseek-chat";
        std::string api_key_ref = "brain-ai/deepseek";
    };

    General  general;
    Ui       ui;
    Output   output;
    Timeout  timeout;
    Error    error;
    Advanced advanced;
    // 旧成员（兼容）：与 `providers["deepseek"]` 互为镜像（加载时同步；保存时作为 deepseek 条的权威）
    Provider deepseek;

    // M_patchB L1（PB2-06）：多 provider **实例参数**（配置表条目 id → 实例参数）
    //  * `config.toml` 的 `[providers.<id>]` 节；**厂商元数据**仍以配置表（`assets/providers.json`
    //    + 用户覆盖层）为准，这里只存「用哪个 provider / 地址覆盖 / 默认模型 / 引用名」
    //  * 旧文件只有 `[providers.deepseek]` 一节 → 加载时自动迁移为映射里的一条（幂等）
    //  * 保存前自动备份 `config.toml.bak`（写失败不覆盖原文件）
    std::map<std::string, Provider> providers;
};

// 读取配置；文件不存在时生成默认配置并落盘。返回是否成功。
bool load_config(const std::filesystem::path& file, Config& out);

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
//    output.ttl_days（PC-05 归档）
//  * ui.grid_size：设计上属"网格间距"，但 vendored imgui-node-editor 的 Style 没有该字段，
//    当前无法生效（如实登记，归口 PD-03 处理：升级库或自绘背景网格）
//  * 节点参数为准：providers.deepseek.*（仅作默认值来源，实际以 ProviderConfig 节点参数为准）
//  * 其余字段在加载时写 `[配置] 以下字段尚未生效：…` 日志，并在使用说明中标注归口补丁
std::vector<std::string> unwired_config_fields();

} // namespace aiwrite
