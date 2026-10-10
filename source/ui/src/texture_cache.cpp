#include "texture_cache.h"

#include "image_decode.h" // M7-04：格式嗅探 + stb/WIC 解码（含可操作错误文案）
#include "log.h"

#include <filesystem>
#include <list>
#include <unordered_map>

// Windows 的 GL/gl.h 依赖 windows.h（WINGDIAPI / APIENTRY）——先引再引 GL
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <GL/gl.h> // glGenTextures / glTexImage2D …（GL 1.1，opengl32 直接导出）

// Windows 的 GL/gl.h 只到 GL 1.1，缺 GL 1.2 的该枚举（值本身是标准定义）
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace aiwrite::ui {
namespace {

// P0（2026-10-09）：容量由 8 → 64，并新增「逐帧保护 + 字节预算」，详见 texture_cache.h 顶部说明。
//  事故根因：同屏卡片图 9 张 > 容量 8 ⇒ 每帧按固定顺序请求 9 张，插入第 9 张时淘汰的
//  恰好是下一帧第一个要用的那张 ⇒ 命中率 0、每帧重解码 9 张（实测 166 ms/帧，掉到个位数 FPS）。
constexpr std::size_t kDefaultCapacity   = 64;
constexpr std::size_t kDefaultByteBudget = 256ull * 1024ull * 1024ull; // 256 MB 解码后纹理
// 硬上限倍数：即使调用方忘了每帧 begin_frame()（g_frame 不前进 → 没有条目"变旧"），
// 超过 容量/预算 的这个倍数后也强制淘汰，保证内存有界。
constexpr std::size_t kHardOverflowFactor = 4;

struct CacheEntry {
    TextureInfo info;
    // 最近使用顺序中自己的位置（g_order.front() = 最近使用）
    std::list<std::string>::iterator order;
    std::size_t bytes = 0; // 纹理字节数（宽×高×4；失败条目为 0）
    std::size_t stamp = 0; // 最近使用所在的帧号（== g_frame 表示「本帧刚用过」→ 本轮不淘汰）
};

std::unordered_map<std::string, CacheEntry> g_cache;
std::list<std::string>                      g_order;
std::size_t                                 g_capacity    = kDefaultCapacity;
std::size_t                                 g_byte_budget = kDefaultByteBudget;
std::size_t                                 g_bytes       = 0; // 当前缓存纹理总字节数

// 逐帧序号（begin_frame() 自增）——「逐帧保护」的依据
std::size_t g_frame = 0;

// 统计（CS-01 诊断；仅渲染线程访问，无需加锁）
std::size_t g_requests  = 0;
std::size_t g_hits      = 0;
std::size_t g_misses    = 0;
std::size_t g_decodes   = 0;
std::size_t g_uploads   = 0;
std::size_t g_evictions = 0;

// 缓存键：路径 + 修改时间 + 大小（文件被替换 → 键变化 → 自动重新加载）
std::string cache_key(const std::string& path)
{
    std::error_code            code;
    const std::filesystem::path file(path);
    const auto write_time = std::filesystem::last_write_time(file, code);
    if (code) {
        return path;
    }
    const auto size = std::filesystem::file_size(file, code);
    return path + "|" + std::to_string(static_cast<long long>(write_time.time_since_epoch().count())) +
           "|" +
           (code ? std::string("?") : std::to_string(static_cast<unsigned long long>(size)));
}

void release_entry(CacheEntry& entry)
{
    if (entry.info.texture != 0) {
        const GLuint texture = static_cast<GLuint>(entry.info.texture);
        glDeleteTextures(1, &texture);
        entry.info.texture = 0;
    }
}

void touch(const std::string& key)
{
    const auto found = g_cache.find(key);
    if (found == g_cache.end()) {
        return;
    }
    g_order.erase(found->second.order);
    g_order.push_front(key);
    found->second.order = g_order.begin();
    found->second.stamp = g_frame; // 本帧用过 → 本轮淘汰保护
}

// 释放一条缓存（同时扣减字节数、从顺序表摘除）
void erase_entry(const std::string& key)
{
    const auto found = g_cache.find(key);
    if (found == g_cache.end()) {
        return;
    }
    release_entry(found->second);
    g_bytes = (g_bytes >= found->second.bytes) ? (g_bytes - found->second.bytes) : 0;
    g_order.erase(found->second.order);
    g_cache.erase(found);
}

void evict_if_needed()
{
    const bool hard_overflow =
        (g_capacity != 0 && g_cache.size() > g_capacity * kHardOverflowFactor) ||
        (g_byte_budget != 0 && g_bytes > g_byte_budget * kHardOverflowFactor);

    while (true) {
        const bool over_count  = (g_capacity != 0) && (g_cache.size() > g_capacity);
        const bool over_budget = (g_byte_budget != 0) && (g_bytes > g_byte_budget);
        if (!over_count && !over_budget) {
            return;
        }

        // 从「最久未使用」一端往回找第一个**不是本帧用过**的条目当牺牲者。
        // 本帧用过的（stamp == g_frame）一律跳过 —— 这正是修复 100% 未命中的关键。
        std::string victim;
        for (auto it = g_order.rbegin(); it != g_order.rend(); ++it) {
            const auto found = g_cache.find(*it);
            if (found == g_cache.end()) {
                continue;
            }
            if (!hard_overflow && found->second.stamp == g_frame) {
                continue;
            }
            victim = *it;
            break;
        }
        if (victim.empty()) {
            return; // 全是本帧刚用过的 → 本帧不再淘汰（下一帧它们自然变旧；硬上限兜底）
        }
        erase_entry(victim);
        ++g_evictions;
    }
}

} // namespace

TextureInfo texture_for(const std::string& path)
{
    if (path.empty()) {
        TextureInfo empty;
        empty.error = "图片路径为空";
        return empty;
    }

    ++g_requests;
    const std::string key = cache_key(path);
    if (const auto found = g_cache.find(key); found != g_cache.end()) {
        ++g_hits;
        touch(key);
        return found->second.info;
    }
    ++g_misses;

    TextureInfo info;
    // P7a-16：顺手按内容嗅探格式（只读文件头）→ 供节点卡片 / 输出面板显示**格式徽标**
    if (const utils::ImageMagic magic = utils::sniffImage(path);
        magic.format != utils::ImageFormat::Unknown) {
        info.format = utils::imageFormatName(magic.format);
    }
    // M7-04：解码统一走 utils::image_decode（stb 优先 / WIC 兜底；错误文案可直接显示）
    ++g_decodes;
    const utils::DecodedImage decoded = utils::decodeImageRgba8(path);
    if (!decoded.error.empty()) {
        info.error = decoded.error;
        log::error("[纹理缓存] " + info.error);
    }
    else {
        GLint max_texture_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
        if (max_texture_size > 0 && (decoded.width > max_texture_size || decoded.height > max_texture_size)) {
            info.error = "图片过大（" + std::to_string(decoded.width) + "×" +
                         std::to_string(decoded.height) + " 超过本机 GL 纹理上限 " +
                         std::to_string(static_cast<int>(max_texture_size)) + "）：请先缩小后再预览";
            log::error("[纹理缓存] " + info.error);
        }
        else {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, decoded.width, decoded.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, decoded.rgba.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            ++g_uploads;
            info.texture = static_cast<unsigned int>(texture);
            info.width   = decoded.width;
            info.height  = decoded.height;
        }
    }

    g_order.push_front(key);
    const std::size_t bytes =
        (info.texture != 0) ? (static_cast<std::size_t>(info.width) *
                               static_cast<std::size_t>(info.height) * 4u)
                            : 0u;
    g_bytes += bytes;
    g_cache[key] = CacheEntry{info, g_order.begin(), bytes, g_frame};
    evict_if_needed();
    return info;
}

bool image_size(const std::string& path, int* width, int* height, std::string* error)
{
    // M7-04：实现搬到 utils::image_decode（stb 优先 / WIC 兜底 + 可操作文案）
    return utils::readImageSize(path, width, height, error);
}

void begin_frame()
{
    // 逐帧标记（契约：每帧渲染开头调用一次，见 ui/app.cpp）
    ++g_frame;
}

void set_texture_capacity(std::size_t capacity)
{
    g_capacity = capacity;
    evict_if_needed();
}

std::size_t texture_capacity()
{
    return g_capacity;
}

void set_texture_byte_budget(std::size_t bytes)
{
    g_byte_budget = bytes;
    evict_if_needed();
}

std::size_t texture_byte_budget()
{
    return g_byte_budget;
}

std::size_t cached_texture_count()
{
    return g_cache.size();
}

TextureCacheStats texture_cache_stats()
{
    TextureCacheStats stats;
    stats.requests    = g_requests;
    stats.hits        = g_hits;
    stats.misses      = g_misses;
    stats.decodes     = g_decodes;
    stats.uploads     = g_uploads;
    stats.evictions   = g_evictions;
    stats.entries     = g_cache.size();
    stats.bytes       = g_bytes;
    stats.capacity    = g_capacity;
    stats.byte_budget = g_byte_budget;
    return stats;
}

void release_textures()
{
    for (auto& item : g_cache) {
        release_entry(item.second);
    }
    g_cache.clear();
    g_order.clear();
    g_bytes = 0;
}

} // namespace aiwrite::ui
