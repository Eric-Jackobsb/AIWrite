#pragma once

// ============================================================================
//  图片纹理缓存（M5-08 纹理缓存；M7-04 起解码走 utils/image_decode）
//
//  * 解码 = stb_image（stb 覆盖的格式）+ Windows WIC（WebP/TIFF/ICO/HEIF…）
//    统一在 utils::image_decode 里做（含按内容嗅探与可操作错误文案）
//  * OpenGL 1.1 纹理上传（`OpenGL::GL` 已链接，无需 glad）
//  * 缓存键 = 路径 + 修改时间 + 大小：文件被替换后自动重新加载
//  * LRU 淘汰（默认 8 张）：超出容量时释放最久未使用的 GL 纹理
//  * **必须在渲染线程调用**（需要当前 GL 上下文）；无界面自检不要调用
//  * 退出/销毁上下文前调用 release_textures()
// ============================================================================

#include <cstddef>
#include <string>

namespace aiwrite::ui {

struct TextureInfo {
    unsigned int texture = 0; // GL 纹理 id（0 = 失败）
    int          width   = 0;
    int          height  = 0;
    std::string  format;      // P7a-16：内容嗅探到的格式名（"PNG" / "WebP"…；未知或失败时为空）
    std::string  error;       // 失败原因（成功时为空）
};

// 取图片纹理（命中缓存直接返回；失败结果也会缓存，避免每帧重试）
TextureInfo texture_for(const std::string& path);

// 读图片尺寸（只解析文件头；不需要 GL 上下文；失败返回 false 并写 error）
bool image_size(const std::string& path, int* width, int* height, std::string* error);

// 容量控制（0 = 不限制）；调小时立即淘汰
void        set_texture_capacity(std::size_t capacity);
std::size_t texture_capacity();
std::size_t cached_texture_count();

// 释放全部纹理（退出前 / GL 上下文销毁前必须调用）
void release_textures();

} // namespace aiwrite::ui
