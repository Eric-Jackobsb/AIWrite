#include "ui/texture_cache.h"

#include "utils/image_decode.h" // M7-04：格式嗅探 + stb/WIC 解码（含可操作错误文案）
#include "utils/log.h"

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

constexpr std::size_t kDefaultCapacity = 8;

struct CacheEntry {
    TextureInfo info;
    // 最近使用顺序中自己的位置（g_order.front() = 最近使用）
    std::list<std::string>::iterator order;
};

std::unordered_map<std::string, CacheEntry> g_cache;
std::list<std::string>                      g_order;
std::size_t                                 g_capacity = kDefaultCapacity;

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
}

void evict_if_needed()
{
    if (g_capacity == 0) {
        return; // 0 = 不限制
    }
    while (g_cache.size() > g_capacity && !g_order.empty()) {
        const std::string oldest = g_order.back();
        g_order.pop_back();
        const auto found = g_cache.find(oldest);
        if (found == g_cache.end()) {
            continue;
        }
        release_entry(found->second);
        g_cache.erase(found);
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

    const std::string key = cache_key(path);
    if (const auto found = g_cache.find(key); found != g_cache.end()) {
        touch(key);
        return found->second.info;
    }

    TextureInfo info;
    // M7-04：解码统一走 utils::image_decode（stb 优先 / WIC 兜底；错误文案可直接显示）
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
            info.texture = static_cast<unsigned int>(texture);
            info.width   = decoded.width;
            info.height  = decoded.height;
        }
    }

    g_order.push_front(key);
    g_cache[key] = CacheEntry{info, g_order.begin()};
    evict_if_needed();
    return info;
}

bool image_size(const std::string& path, int* width, int* height, std::string* error)
{
    // M7-04：实现搬到 utils::image_decode（stb 优先 / WIC 兜底 + 可操作文案）
    return utils::readImageSize(path, width, height, error);
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

std::size_t cached_texture_count()
{
    return g_cache.size();
}

void release_textures()
{
    for (auto& item : g_cache) {
        release_entry(item.second);
    }
    g_cache.clear();
    g_order.clear();
}

} // namespace aiwrite::ui
