#include "utils/config.h"

#include "utils/log.h"

#include <toml++/toml.hpp>

#include <fstream>
#include <string>

namespace aiwrite {
namespace {

template <typename T>
void read_value(const toml::table& table, const char* key, T& target)
{
    if (const toml::node* node = table.get(key); node != nullptr) {
        if (const auto value = node->value<T>()) {
            target = *value;
        }
    }
}

void read_general(const toml::table& t, Config::General& s)
{
    read_value(t, "language", s.language);
    read_value(t, "startup", s.startup);
}

void read_ui(const toml::table& t, Config::Ui& s)
{
    read_value(t, "show_node_library", s.show_node_library);
    read_value(t, "show_property_panel", s.show_property_panel);
    read_value(t, "show_console", s.show_console);
    read_value(t, "show_output_window", s.show_output_window);
    read_value(t, "console_height", s.console_height);
    read_value(t, "show_grid", s.show_grid);
    read_value(t, "grid_size", s.grid_size);
    read_value(t, "running_animation", s.running_animation);
}

void read_output(const toml::table& t, Config::Output& s)
{
    read_value(t, "archive_dir", s.archive_dir);
    read_value(t, "ttl_days", s.ttl_days);
    read_value(t, "auto_open_on_complete", s.auto_open_on_complete);
    read_value(t, "keep_history", s.keep_history);
    read_value(t, "max_history", s.max_history);
}

void read_timeout(const toml::table& t, Config::Timeout& s)
{
    read_value(t, "connect_ms", s.connect_ms);
    read_value(t, "first_byte_ms", s.first_byte_ms);
    read_value(t, "stream_idle_ms", s.stream_idle_ms);
}

void read_error(const toml::table& t, Config::Error& s)
{
    read_value(t, "retry_enabled", s.retry_enabled);
    read_value(t, "retry_count", s.retry_count);
    read_value(t, "retry_interval_ms", s.retry_interval_ms);
}

void read_advanced(const toml::table& t, Config::Advanced& s)
{
    read_value(t, "config_path", s.config_path);
    read_value(t, "log_dir", s.log_dir);
    read_value(t, "log_ttl_days", s.log_ttl_days);
}

void read_provider(const toml::table& t, Config::Provider& s)
{
    read_value(t, "provider", s.provider);
    read_value(t, "mode", s.mode);
    read_value(t, "api_base", s.api_base);
    read_value(t, "model", s.model);
    read_value(t, "api_key_ref", s.api_key_ref);
}

template <typename T>
void write_value(toml::table& table, const char* key, const T& value)
{
    table.insert(key, value);
}

toml::table to_table(const Config& c)
{
    toml::table root;
    write_value(root, "config_version", c.config_version);

    toml::table general;
    write_value(general, "language", c.general.language);
    write_value(general, "startup", c.general.startup);
    root.insert("general", std::move(general));

    toml::table ui;
    write_value(ui, "show_node_library", c.ui.show_node_library);
    write_value(ui, "show_property_panel", c.ui.show_property_panel);
    write_value(ui, "show_console", c.ui.show_console);
    write_value(ui, "show_output_window", c.ui.show_output_window);
    write_value(ui, "console_height", c.ui.console_height);
    write_value(ui, "show_grid", c.ui.show_grid);
    write_value(ui, "grid_size", c.ui.grid_size);
    write_value(ui, "running_animation", c.ui.running_animation);
    root.insert("ui", std::move(ui));

    toml::table output;
    write_value(output, "archive_dir", c.output.archive_dir);
    write_value(output, "ttl_days", c.output.ttl_days);
    write_value(output, "auto_open_on_complete", c.output.auto_open_on_complete);
    write_value(output, "keep_history", c.output.keep_history);
    write_value(output, "max_history", c.output.max_history);
    root.insert("output", std::move(output));

    toml::table timeout;
    write_value(timeout, "connect_ms", c.timeout.connect_ms);
    write_value(timeout, "first_byte_ms", c.timeout.first_byte_ms);
    write_value(timeout, "stream_idle_ms", c.timeout.stream_idle_ms);
    root.insert("timeout", std::move(timeout));

    toml::table error;
    write_value(error, "retry_enabled", c.error.retry_enabled);
    write_value(error, "retry_count", c.error.retry_count);
    write_value(error, "retry_interval_ms", c.error.retry_interval_ms);
    root.insert("error", std::move(error));

    toml::table advanced;
    write_value(advanced, "config_path", c.advanced.config_path);
    write_value(advanced, "log_dir", c.advanced.log_dir);
    write_value(advanced, "log_ttl_days", c.advanced.log_ttl_days);
    root.insert("advanced", std::move(advanced));

    toml::table deepseek;
    write_value(deepseek, "provider", c.deepseek.provider);
    write_value(deepseek, "mode", c.deepseek.mode);
    write_value(deepseek, "api_base", c.deepseek.api_base);
    write_value(deepseek, "model", c.deepseek.model);
    write_value(deepseek, "api_key_ref", c.deepseek.api_key_ref);

    toml::table providers;
    providers.insert("deepseek", std::move(deepseek));
    root.insert("providers", std::move(providers));

    return root;
}

} // namespace

bool load_config(const std::filesystem::path& file, Config& out)
{
    out = Config{};

    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        log::info("配置文件不存在，生成默认配置: " + file.string());
        return save_config(file, out);
    }

    try {
        const toml::table root = toml::parse_file(file.string());
        read_value(root, "config_version", out.config_version);

        if (const toml::table* section = root["general"].as_table()) read_general(*section, out.general);
        if (const toml::table* section = root["ui"].as_table())      read_ui(*section, out.ui);
        if (const toml::table* section = root["output"].as_table())  read_output(*section, out.output);
        if (const toml::table* section = root["timeout"].as_table()) read_timeout(*section, out.timeout);
        if (const toml::table* section = root["error"].as_table())   read_error(*section, out.error);
        if (const toml::table* section = root["advanced"].as_table()) read_advanced(*section, out.advanced);

        if (const toml::table* providers = root["providers"].as_table()) {
            if (const toml::table* deepseek = (*providers)["deepseek"].as_table()) {
                read_provider(*deepseek, out.deepseek);
            }
        }

        log::info("配置已加载: " + file.string() + " (config_version=" +
                  std::to_string(out.config_version) + ")");
        return true;
    }
    catch (const toml::parse_error& ex) {
        log::error(std::string("配置文件解析失败: ") + ex.what());
        return false;
    }
    catch (const std::exception& ex) {
        log::error(std::string("读取配置异常: ") + ex.what());
        return false;
    }
}

bool save_config(const std::filesystem::path& file, const Config& config)
{
    try {
        std::error_code ec;
        if (file.has_parent_path()) {
            std::filesystem::create_directories(file.parent_path(), ec);
        }

        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (!stream) {
            log::error("无法写入配置文件: " + file.string());
            return false;
        }

        stream << "# AIwrite 配置文件（字段见设计文档 20.2 节）\n"
               << "# 程序启动时读取，设置面板（M6）保存\n\n"
               << to_table(config);

        if (!stream.good()) {
            log::error("配置文件写入不完整: " + file.string());
            return false;
        }

        log::info("配置已保存: " + file.string());
        return true;
    }
    catch (const std::exception& ex) {
        log::error(std::string("写入配置异常: ") + ex.what());
        return false;
    }
}

} // namespace aiwrite
