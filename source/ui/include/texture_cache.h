#pragma once

// ============================================================================
//  图片纹理缓存（M5-08 纹理缓存；M7-04 起解码走 utils/image_decode）
//
//  * 解码 = stb_image（stb 覆盖的格式）+ Windows WIC（WebP/TIFF/ICO/HEIF…）
//    统一在 utils::image_decode 里做（含按内容嗅探与可操作错误文案）
//  * OpenGL 1.1 纹理上传（`OpenGL::GL` 已链接，无需 glad）
//  * 缓存键 = 路径 + 修改时间 + 大小：文件被替换后自动重新加载
//  * LRU 淘汰（默认 64 条 / 256 MB，**+ 逐帧保护**）：见下「P0 事故」说明
//  * **必须在渲染线程调用**（需要当前 GL 上下文）；无界面自检不要调用
//  * 退出/销毁上下文前调用 release_textures()
//
//  ---- P0 事故与修复（2026-10-09，个位数 FPS）----
//  容量曾是 8，而 `testAllAI.json` 的同屏卡片图有 9 张：每帧按固定顺序请求这 9 张，
//  第 9 张插入时淘汰的**恰好是下一帧第一个要用的那张** → 命中率恒为 0，
//  每帧把 9 张 JPEG 全部重新解码（实测 stb 约 166 ms/帧，另加 32 MB/帧 纹理上传）
//  ⇒ 界面掉到 5~6 FPS。修复两手：
//    ① 默认容量 8 → 64，并加字节预算（256 MB）；
//    ② `begin_frame()` 逐帧标记：**本帧用过的条目不淘汰**（容量小于同屏卡片数时
//       也只"超一帧"，不会每帧互相顶掉）。
//  调用契约：渲染循环每帧开头调用一次 begin_frame()（见 ui/app.cpp），
//  否则「逐帧保护」退化为「永不淘汰」（已由 4× 硬上限兜底，不会无限增长）。
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
TextureInfo textureFor(const std::string& path);

// 逐帧标记：渲染循环**每帧开头**调用一次（见 ui/app.cpp）
//  * 标记本帧用到的纹理为「保护」，本轮淘汰不碰它们
//  * 作用：同屏卡片数 > 容量时也不会出现「LRU 互相顶掉 → 每帧 100% 未命中」
void beginFrame();

// 读图片尺寸（只解析文件头；不需要 GL 上下文；失败返回 false 并写 error）
bool imageSize(const std::string& path, int* width, int* height, std::string* error);

// 容量控制（0 = 不限制）；调小时立即淘汰
void        setTextureCapacity(std::size_t capacity);
std::size_t textureCapacity();
std::size_t cachedTextureCount();

// 字节预算（解码后纹理字节数；0 = 不限制；默认 256 MB）——防止「大图工作流」吃爆显存
void        setTextureByteBudget(std::size_t bytes);
std::size_t textureByteBudget();

// 统计（CS-01 诊断用；仅渲染线程读写，无锁）
//  * 累计值单调递增，调用方自己取差值算「近 N 秒」的请求/命中/解码次数
//  * `decodes` 是**每帧重解码**事故的直接指标（稳态应恒为 0）
struct TextureCacheStats {
    std::size_t requests   = 0; // texture_for 调用次数
    std::size_t hits       = 0; // 命中缓存
    std::size_t misses     = 0; // 未命中（进入解码流程）
    std::size_t decodes    = 0; // 实际解码次数（stb/WIC）
    std::size_t uploads    = 0; // 实际 glTexImage2D 次数
    std::size_t evictions  = 0; // 淘汰次数
    std::size_t entries    = 0; // 当前条目数
    std::size_t bytes      = 0; // 当前纹理字节数（宽×高×4）
    std::size_t capacity   = 0; // 当前条目容量（0 = 不限制）
    std::size_t byte_budget = 0; // 当前字节预算（0 = 不限制）
};
TextureCacheStats textureCacheStats();

// 释放全部纹理（退出前 / GL 上下文销毁前必须调用）
void releaseTextures();

} // namespace aiwrite::ui
