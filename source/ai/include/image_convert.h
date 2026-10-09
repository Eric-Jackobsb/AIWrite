#pragma once

// ============================================================================
//  图片「准备」层：格式识别 → 按站点白名单/限额 → （必要时）缩放 + 转码（M8-14）
//
//  为什么需要它：站点对**可上传格式 / 体积 / 边长**各有要求（豆包 accept 串里同时有
//  `.png/.jpeg/.jpg/.webp/.pdf/.docx/.mp3/.wav` 等 —— 网页版还会给「图片」单独的限额）。
//  把「站点要什么格式」交给站点单元，把「怎么转」交给本模块。
//
//  复用（**不做重复实现**）：
//    * 嗅探 = `utils::sniffImage` / `sniffImageBytes`（内容优先，扩展名可能骗人）
//    * 解码 = `utils::decodeImageRgba8`（stb 优先 / WIC 兜底）
//    * ⚠️ `utils/image_decode.*` 是**冻结区**（`M7-D7` / 不变量 `I17`）：本模块**只调用、不修改**
//  新增（零新增依赖 —— 两个头文件随 vcpkg 的 stb 端口已在树里）：
//    * `stb_image_resize.h` → 长边缩放（`stbir_resize_uint8`）
//    * `stb_image_write.h`  → 编码 PNG / JPG（`stbi_write_png` / `stbi_write_jpg`）
//
//  转换只在**必要时**发生（白名单不含 / 体积超限 / 长边超限），且每次都产出一行 note
//  （"webp→png（站点白名单不含 webp）"）→ 由调用方写进 Console（可追溯，不静默）。
//
//  离线可断言：白名单解析 / 放行判定 / 转换判定 / 真实转码（用临时文件）—— 全部无网络、无窗口。
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "ai/upload/upload_contract.h"
#include "utils/image_decode.h"

namespace aiwrite::ai {

// 站点 `accept` 字符串 → **图片子集**白名单（".png,.jpeg,.jpg,.webp,.pdf,.docx,.mp3"
//  → {"png","jpeg","jpg","webp"}）
//  * 只保留图片扩展名：站点 accept 里常混着文档 / 音频 —— 绝不能把 mp3 当图上传
//  * 大小写不敏感、去前导点、去空项；去重并**保持原顺序**（便于人工核对）
std::vector<std::string> image_exts_from_accept(const std::string& accept);

// 白名单是否放行该文件（纯函数）
//  * `accept` 空 → 放行（站点未声明 = 不限制）
//  * `jpeg` / `jpg` / `jpe` 视为同一族（互相放行）
//  * 内容嗅探为 Unknown 时看扩展名（`ext_label` 含点，例 ".png"）
bool accept_allows(const std::vector<std::string>& accept, utils::ImageFormat format,
                   const std::string& ext_label);

// 是否需要转换（纯函数；`*reason` 写人话，例："webp 不在站点白名单内"）
//  * 判据顺序：白名单 → 体积（`max_bytes`）
//  * **长边**（`max_edge`）需要像素尺寸，不在本函数内 —— 由 `convert_for_upload` 复核
//    （那里会 `readImageSize`；本函数保持零 IO、可离线断言）
bool need_convert(const utils::ImageMagic& magic, const UploadPrepareOptions& options,
                  std::size_t file_bytes, std::string* reason);

// 准备结果
struct ConvertOutcome {
    bool        ok        = false;
    std::string path;              // 交付路径（未转换时 = 入参数原样）
    bool        converted = false; // 是否真的产出了新文件
    std::string note;              // 转换说明（未转换时为空）
    std::string error;             // ok=false 时的**可操作**原因（`I21`）
};

// 执行准备：嗅探 → （必要时）解码为 RGBA8 → 缩放 → 转码 → 写临时文件
//  * 不需要转换时**不复制、不解码**（零开销路径，大图友好）
//  * `target_ext`：png = 无损（默认）；jpg = 有损但有 1..100 质量（本实现固定 90）
//  * 失败文案含：原路径 / 嗅探到的格式 / 该做的下一步（如「装 Webp 图像扩展」）
ConvertOutcome convert_for_upload(const std::string& local_path,
                                  const UploadPrepareOptions& options);

// 默认临时目录：`%TEMP%\aiwrite-upload`（取不到临时目录 → `~/.brain-ai/tmp/upload`）
//  * 目录不存在时由 `convert_for_upload` 按需创建
std::string default_upload_tmp_dir();

// 离线自检（返回**失败项数**；`passed_out` 回传通过项数 → 调用方并入总计数）
// 覆盖：accept 解析（含非图片混入 / 大小写 / 去重）/ 放行判定（jpeg↔jpg / Unknown 回退 /
//       空白名单）/ 转换判定（白名单 / 体积 / 长边）/ 真实转码（webp→png、超限缩放、
//       扩展名骗人）/ 失败路径（缺文件 / 非图片 / 空文件 → 可操作文案）
int image_convert_selftest(int* passed_out = nullptr);

} // namespace aiwrite::ai
