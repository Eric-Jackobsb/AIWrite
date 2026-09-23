#pragma once

// ============================================================================
//  文件 / 目录选择对话框（设计文档 §2.2：nativefiledialog-extended）
//
//  * 在参数面板的 File / Directory 控件里使用
//  * 取消或失败返回空字符串；错误写日志
//  * 注意：NFD 的对话框标题由系统决定，无法自定义
// ============================================================================

#include <string>
#include <vector>

namespace aiwrite::utils {

struct FileFilter {
    std::string label;   // 显示名，如 "图片"
    std::string pattern; // 扩展名列表，如 "png,jpg,jpeg"；全部文件用 "*"
};

// 选择单个文件；取消/失败返回空字符串
std::string open_file(const std::vector<FileFilter>& filters = {},
                      const std::string& default_path = {});

// 保存文件对话框；取消/失败返回空字符串（default_name 为默认文件名）
std::string save_file(const std::vector<FileFilter>& filters = {},
                      const std::string& default_path = {},
                      const std::string& default_name = {});

// 选择目录；取消/失败返回空字符串
std::string pick_folder(const std::string& default_path = {});

// 依赖是否可用（NFD 能否初始化）
bool file_dialog_available();

} // namespace aiwrite::utils
