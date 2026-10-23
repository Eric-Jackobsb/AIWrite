#include "config.h"

#include "log.h"

#include <toml++/toml.hpp>

#include <fstream>
#include <mutex>
#include <string>
#include <utility>

namespace aiwrite {
namespace {

// PA-07：进程级配置缓存（只读访问为主）
// ARCH-A7：原先是两条文件级 static（`g_config_mutex` / `g_config`）⇒ 归拢为显式状态对象，
// 单一定义点 = `configStore()`（函数作用域单实例，C++11 起初始化线程安全）。
struct ConfigStore {
    std::mutex mutex;
    Config     value;
};

ConfigStore& configStore()
{
    static ConfigStore store;
    return store;
}

} // namespace

const Config& appConfig()
{
    std::lock_guard<std::mutex> lock(configStore().mutex);
    return configStore().value;
}

void setAppConfig(Config config)
{
    std::lock_guard<std::mutex> lock(configStore().mutex);
    configStore().value = std::move(config);
}

// PA-08：尚未接线的配置字段清单（见 config.h 注释；加载时写日志 + 文档标注）
std::vector<std::string> unwiredConfigFields()
{
    return {
        // `PQ-18②`（2026-10-09）：general.language / general.startup **已移除** ——
        //  「界面语言切换 / 启动行为」不做（i18n 决策见 roadmap.md §十）；旧 [general] 节被静默忽略
        "ui.grid_size",                 // 网格间距：vendored imgui-node-editor 的 Style 无此字段（PD-03 处理）
        // 口径变更（2026-10-10 · M6）：以下两项**已接线**，故从清单移除（不再是「改了没效果」）——
        //  * `output.auto_open_on_complete` → EditorController::archiveRunOutputs() 归档成功后打开该目录
        //  * `advanced.log_ttl_days`        → 启动时清理 logs/ 下超过该天数的旧日志（不含 app.log / workflow.log）
        // 口径变更（2026-10-11 · M_patchAB 批 4）：`timeout.*` / `error.*` **已接线**，故从清单移除 ——
        //  * `timeout.connect_ms` / `first_byte_ms` → 官方 API 的**默认**连接 / 读超时
        //    （**表未声明** `limits.*` 时生效；表优先，见 nodes/local_nodes.cpp 与 PB2-04 口径）
        //  * `error.retry_enabled / retry_count / retry_interval_ms` → 官方 API 节点重试
        //    （只对**可重试**失败重试：429 / 5xx / 网络超时；凭证 / 余额 / 请求不合法**不重试**）
        "providers.deepseek.*",     // 仅作默认值来源：实际以 ProviderConfig 节点参数为准
                                    // （PB2-06 起支持 [providers.<id>] 多节；厂商元数据以配置表为准）
    };
}

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
    read_value(t, "window_width", s.window_width);
    read_value(t, "window_height", s.window_height);
    read_value(t, "window_pos_x", s.window_pos_x);
    read_value(t, "window_pos_y", s.window_pos_y);
    read_value(t, "window_maximized", s.window_maximized);
    read_value(t, "first_run_done", s.first_run_done); // M6 首启引导（缺键 → 保持默认 false）
    // PC-01/02：自动保存与崩溃恢复（缺键 → 保持默认：开 / 3 秒 / 3 份 / 会提示）
    read_value(t, "autosave_enabled", s.autosave_enabled);
    read_value(t, "autosave_idle_sec", s.autosave_idle_sec);
    read_value(t, "autosave_keep", s.autosave_keep);
    read_value(t, "autosave_restore_prompt", s.autosave_restore_prompt);
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

// M_patchAB §2.3（`PB-06` 剩余）：`[network]`
void read_network(const toml::table& t, Config::Network& s)
{
    read_value(t, "proxy", s.proxy);
    read_value(t, "proxy_from_env", s.proxy_from_env);
    read_value(t, "no_proxy", s.no_proxy);
    read_value(t, "insecure_tls", s.insecure_tls);
}

// M_patchAB §2.3（`PB-06` 剩余）：`[credentials]`
void read_credentials(const toml::table& t, Config::Credentials& s)
{
    read_value(t, "backend", s.backend);
    read_value(t, "dir", s.dir);
    read_value(t, "ttl_days", s.ttl_days);
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

    // `PQ-18②`（2026-10-09）：不再写 `[general]` 节（general.language / general.startup 已移除）

    toml::table ui;
    write_value(ui, "show_node_library", c.ui.show_node_library);
    write_value(ui, "show_property_panel", c.ui.show_property_panel);
    write_value(ui, "show_console", c.ui.show_console);
    write_value(ui, "show_output_window", c.ui.show_output_window);
    write_value(ui, "console_height", c.ui.console_height);
    write_value(ui, "show_grid", c.ui.show_grid);
    write_value(ui, "grid_size", c.ui.grid_size);
    write_value(ui, "running_animation", c.ui.running_animation);
    write_value(ui, "window_width", c.ui.window_width);
    write_value(ui, "window_height", c.ui.window_height);
    write_value(ui, "window_pos_x", c.ui.window_pos_x);
    write_value(ui, "window_pos_y", c.ui.window_pos_y);
    write_value(ui, "window_maximized", c.ui.window_maximized);
    write_value(ui, "first_run_done", c.ui.first_run_done); // M6 首启引导标记
    // PC-01/02：自动保存与崩溃恢复（M_patchAB §4）
    write_value(ui, "autosave_enabled", c.ui.autosave_enabled);
    write_value(ui, "autosave_idle_sec", c.ui.autosave_idle_sec);
    write_value(ui, "autosave_keep", c.ui.autosave_keep);
    write_value(ui, "autosave_restore_prompt", c.ui.autosave_restore_prompt);
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

    // M_patchAB §2.3（`PB-06` 剩余）：`[network]`（代理 / 自签证书）
    toml::table network;
    write_value(network, "proxy", c.network.proxy);
    write_value(network, "proxy_from_env", c.network.proxy_from_env);
    write_value(network, "no_proxy", c.network.no_proxy);
    write_value(network, "insecure_tls", c.network.insecure_tls);
    root.insert("network", std::move(network));

    // M_patchAB §2.3（`PB-06` 剩余）：`[credentials]`（凭据库三参数）
    toml::table credentials;
    write_value(credentials, "backend", c.credentials.backend);
    write_value(credentials, "dir", c.credentials.dir);
    write_value(credentials, "ttl_days", c.credentials.ttl_days);
    root.insert("credentials", std::move(credentials));

    toml::table providers;
    // M_patchB L1（PB2-06）：多 provider —— 写出全部 `[providers.<id>]`；
    //  * 旧成员 `deepseek` 始终作为 `deepseek` 条目的权威（兼容既有代码/自检）
    //  * 映射为空时也写出 `[providers.deepseek]`（保持旧文件形态不变）
    std::map<std::string, Config::Provider> entries = c.providers;
    entries["deepseek"]                             = c.deepseek;
    for (const auto& item : entries) {
        toml::table section;
        write_value(section, "provider", item.second.provider);
        write_value(section, "mode", item.second.mode);
        write_value(section, "api_base", item.second.api_base);
        write_value(section, "model", item.second.model);
        write_value(section, "api_key_ref", item.second.api_key_ref);
        providers.insert(item.first, std::move(section));
    }
    root.insert("providers", std::move(providers));

    return root;
}

} // namespace

// ---- M_patchAB §4（`PC-04`）：配置文件版本迁移（纯逻辑；`save_config` 的护栏同源）----
int currentConfigVersion()
{
    return 1;
}

ConfigMigrationOutcome migrateConfig(Config& config)
{
    ConfigMigrationOutcome outcome;
    outcome.from = config.config_version;
    outcome.to   = currentConfigVersion();

    if (config.config_version <= 0) {
        config.config_version               = outcome.to; // 老文件（无 config_version 键）
        config.version_newer_than_supported = false;
        outcome.migrated                    = true;
        outcome.notes.emplace_back("配置文件没有 config_version（老文件）⇒ 按 " +
                                   std::to_string(outcome.to) + " 处理");
        return outcome;
    }
    if (config.config_version > outcome.to) {
        config.version_newer_than_supported = true;
        outcome.future                      = true;
        outcome.notes.emplace_back(
            "配置文件版本 " + std::to_string(config.config_version) + " 比本程序支持的 " +
            std::to_string(outcome.to) +
            " 更新：已按已知字段加载，**拒绝覆盖写回**（原文件保持不动，保存会被拒绝）");
        log::warn("[配置] " + outcome.notes.back());
        return outcome;
    }
    config.version_newer_than_supported = false;
    return outcome;
}


bool loadConfig(const std::filesystem::path& file, Config& out)
{
    out = Config{};

    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        log::info("配置文件不存在，生成默认配置: " + file.string());
        return saveConfig(file, out);
    }
    try {
        const toml::table root = toml::parse_file(file.string());
        read_value(root, "config_version", out.config_version);

        if (const toml::table* section = root["ui"].as_table())      read_ui(*section, out.ui);
        if (const toml::table* section = root["output"].as_table())  read_output(*section, out.output);
        if (const toml::table* section = root["timeout"].as_table()) read_timeout(*section, out.timeout);
        if (const toml::table* section = root["error"].as_table())   read_error(*section, out.error);
        if (const toml::table* section = root["advanced"].as_table()) read_advanced(*section, out.advanced);
        // M_patchAB §2.3（`PB-06` 剩余）：`[network]` / `[credentials]`（缺节 ⇒ 全部取默认值）
        if (const toml::table* section = root["network"].as_table()) read_network(*section, out.network);
        if (const toml::table* section = root["credentials"].as_table())
            read_credentials(*section, out.credentials);

        // M_patchB L1（PB2-06）：多 provider 实例参数 —— 任意 `[providers.<id>]` 节都读入映射
        // （旧文件的 `[providers.deepseek]` 据此自动迁移，幂等）；随后与旧成员 `deepseek` 同步镜像
        if (const toml::table* providers = root["providers"].as_table()) {
            for (const auto& item : *providers) {
                const toml::table* section = item.second.as_table();
                if (section == nullptr) {
                    continue;
                }
                Config::Provider parsed;
                read_provider(*section, parsed);
                out.providers[std::string(item.first.str())] = parsed;
            }
        }
        if (const auto it = out.providers.find("deepseek"); it != out.providers.end()) {
            out.deepseek = it->second; // 镜像（旧代码读 Config::deepseek）
        }

        // M_patchAB §4（PC-04）：版本迁移（补齐版本号 / 未来版本标记）
        const ConfigMigrationOutcome migration = migrateConfig(out);
        for (const std::string& note : migration.notes) {
            log::info("[配置迁移] " + note);
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

bool saveConfig(const std::filesystem::path& file, const Config& config)
{
    // ---- M_patchAB §4（PC-04）：**拒绝降级写回** ----
    //  * 磁盘上的配置文件版本比本程序新（`load_config` 已标记）⇒ 保存会被拒绝：
    //    否则会先把用户的新版配置备份成 `.bak`、再用旧格式覆盖主文件（等于降级写坏）
    if (config.version_newer_than_supported) {
        log::error("[配置] 保存被拒绝：磁盘配置版本比本程序支持的 " +
                   std::to_string(currentConfigVersion()) +
                   " 更新（请升级 AIwrite；原文件未改动）");
        return false;
    }
    try {
        std::error_code ec;
        if (file.has_parent_path()) {
            std::filesystem::create_directories(file.parent_path(), ec);
        }

        // M_patchB L1（PB2-06）：写盘前备份原文件（旧配置迁移/写失败时都不丢原配置）
        if (std::filesystem::exists(file, ec)) {
            const std::filesystem::path backup = file.string() + ".bak";
            std::filesystem::copy_file(file, backup,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                log::warn("配置备份失败（继续保存）: " + backup.string() + "（" + ec.message() + "）");
                ec.clear();
            }
        }

        // 先写临时文件、再原子替换：任何写入失败都不会破坏原配置（VB2-06）
        const std::filesystem::path temp = file.string() + ".tmp";
        {
            std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
            if (!stream) {
                log::error("无法写入配置文件: " + temp.string());
                return false;
            }

            stream << "# AIwrite 配置文件（字段见设计文档 20.2 节）\n"
                   << "# 程序启动时读取，设置面板（M6）保存\n\n"
                   << to_table(config);

            if (!stream.good()) {
                log::error("配置文件写入不完整: " + temp.string() + "（原文件保持不变）");
                stream.close();
                std::filesystem::remove(temp, ec);
                return false;
            }
        }

        std::filesystem::rename(temp, file, ec);
        if (ec) {
            log::error("配置替换失败: " + file.string() + "（" + ec.message() + "）");
            std::filesystem::remove(temp, ec);
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
