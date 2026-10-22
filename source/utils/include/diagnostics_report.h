#pragma once

// ============================================================================
//  诊断报告（M6 `PM-04`：非技术用户「一键复制诊断信息」）
//
//  目标：**用户不用懂技术**——出问题时点一次「帮助 → 复制诊断信息」，
//        把粘贴出的文本发给支持者，就能知道：装的哪个版本 / 系统与依赖是否满足 /
//        配置与日志在哪 / 站点表加载是否正常。
//
//  纪律（与 AI_README §3 取证纪律一致）：
//   * **只读**：不写盘、不发网络、不改配置、不创建目录（不存在就如实写「不存在」）
//   * **不泄密**：不输出任何凭据明文 / 密文 / Cookie 值；凭据**只出条数**
//   * **可离线断言**：`collect_diagnostics()` 是纯读取函数；`api_probe --diag-assert`
//     对其输出做 PASS/FAIL 断言（不依赖网络与 GUI）
//   * 与 `utils/diagnostics.{h,cpp}` **区分**：那是运行期首异常计数 + 内存采样，
//     本文件是「给用户复制走的环境报告」，两者不共用实现
//
//  输出格式（人读 + 机器可断言）：`[节名]` 行 + 若干 `键 = 值` 行；
//   * `key_value("程序版本")` 取「键 = 值」的右侧，便于断言
// ============================================================================

#include <string>
#include <vector>

namespace aiwrite::utils {

struct DiagnosticsReport {
    std::vector<std::string> lines; // 已格式化（UTF-8，不含换行符）

    // 全部行以 '\n' 连接（含结尾换行）；可直接进剪贴板 / 日志
    std::string text() const;

    // 是否包含子串（断言用）
    bool contains(const std::string& needle) const;

    // 取「键 = 值」中的值（去掉键与空白）；键不存在返回空串
    //  * 只取**第一个**匹配；值里允许再出现 " = "
    std::string keyValue(const std::string& key) const;
};

// 采集诊断信息（只读；任何单项失败都降级为一行说明，不抛异常）
DiagnosticsReport collectDiagnostics();

// 便捷：collect_diagnostics().text()
std::string diagnosticsReportText();

} // namespace aiwrite::utils
