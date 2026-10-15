#pragma once

// ============================================================================
//  图片预处理（M_8nodes_extension L1-c · §4.2）：**用户主动参数**驱动的缩放 / 裁剪
//
//  * 执行顺序：读取 → 裁剪 → 缩放 → 写临时 PNG（文档 §4.2）
//  * **复用**（不重复实现）：
//      - 解码 = `utils::decodeImageRgba8`（stb 优先 / WIC 兜底 —— **只调用、不修改**，守 I17）
//      - 缩放 = `stbir_resize_uint8`（与 `ai/image_convert` 同族底层，零新增依赖）
//  * **与站点上传**的 `image_convert`（白名单 / 限额驱动）**无关**：不应用站点判定策略，
//    否则用户主动参数会被站点限额改写（文档 §4.2 ⚠️ 复用注意）
//  * 无参数变化时**零开销**（原样返回，不复制、不解码、不写盘）
//  * 裁剪 = 新增（现有实现无裁剪能力）；只操作 RGBA8 像素缓冲，**不碰** WIC
// ============================================================================

#include <string>

namespace aiwrite::ai {

// 预处理参数（N-02 图片输入 / N-07 图片理解共用；注册表参数见 node_registry.cpp）
struct ImagePreprocessParams {
    std::string resize_mode = "none"; // none / fit / fill
    int         resize_max_width  = 0; // 0 = 不限制
    int         resize_max_height = 0; // 0 = 不限制
    bool        crop_enabled      = false;
    int         crop_x = 0; // 裁剪区域（相对原图；w/h <= 0 时取右侧/下侧剩余）
    int         crop_y = 0;
    int         crop_w = 0;
    int         crop_h = 0;
};

struct PreprocessOutcome {
    bool        ok      = false;
    std::string path;    // 预处理后文件（未变化 = 原路径原样返回）
    bool        changed = false; // 是否真的产出了新文件
    std::string note;    // 如「裁剪 400x300 → 缩放 200x150」（写进 Console，可追溯）
    std::string error;   // ok=false 时的可操作原因
};

// 预处理单张图片（阻塞；写临时 PNG 到 upload 临时目录）
PreprocessOutcome preprocess_image(const std::string& local_path,
                                   const ImagePreprocessParams& params);

// 离线自检（返回**失败项数**；`passed_out` 回传通过项数 → 调用方并入总计数）
// 覆盖：零开销路径 / 裁剪 / 缩放（fit / fill）/ 裁剪+缩放组合 / 缺文件失败文案
int image_preprocess_selftest(int* passed_out = nullptr);

} // namespace aiwrite::ai
