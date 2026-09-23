#pragma once

#include <filesystem>
#include <string>

// 配置管理（字段与设计文档 20.2 节 config.toml 示例一致）
namespace aiwrite {

struct Config {
    int config_version = 1;

    struct General {
        std::string language = "zh-CN";
        std::string startup  = "welcome";
    };

    struct Ui {
        bool show_node_library   = false;
        bool show_property_panel = false;
        bool show_console        = true;
        bool show_output_window  = false;
        int  console_height      = 120;
        bool show_grid           = true;
        int  grid_size           = 20;
        bool running_animation   = true;
    };

    struct Output {
        std::string archive_dir = "~/.brain-ai/outputs";
        int  ttl_days           = 30;
        bool auto_open_on_complete = false;
        bool keep_history       = false;
        int  max_history        = 10;
    };

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
    Provider deepseek;
};

// 读取配置；文件不存在时生成默认配置并落盘。返回是否成功。
bool load_config(const std::filesystem::path& file, Config& out);

// 写入配置（自动创建父目录）
bool save_config(const std::filesystem::path& file, const Config& config);

} // namespace aiwrite
