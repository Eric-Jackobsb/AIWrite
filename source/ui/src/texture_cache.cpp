#include "texture_cache.h"

#include "image_decode.h" // M7-04：格式嗅探 + stb/WIC 解码（含可操作错误文案）
#include "log.h"

#include <filesystem>

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

// 缓存键：路径 + 修改时间 + 大小（文件被替换 → 键变化 → 自动重新加载）
std::string cacheKey(const std::string& path)
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

} // namespace

// ---------------------------------------------------------------------------
//  内部：条目生命周期（ARCH-A6 起这些原 g_* 文件级变量都是本对象成员）
// ---------------------------------------------------------------------------
void TextureCache::releaseEntry(Entry& entry)
{
    if (entry.info.texture != 0) {
        const GLuint texture = static_cast<GLuint>(entry.info.texture);
        glDeleteTextures(1, &texture);
        entry.info.texture = 0;
    }
}

void TextureCache::touch(const std::string& key)
{
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return;
    }
    order_.erase(found->second.order);
    order_.push_front(key);
    found->second.order = order_.begin();
    found->second.stamp = frame_; // 本帧用过 → 本轮淘汰保护
}

// 释放一条缓存（同时扣减字节数、从顺序表摘除）
void TextureCache::eraseEntry(const std::string& key)
{
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return;
    }
    releaseEntry(found->second);
    bytes_ = (bytes_ >= found->second.bytes) ? (bytes_ - found->second.bytes) : 0;
    order_.erase(found->second.order);
    entries_.erase(found);
}

void TextureCache::evictIfNeeded()
{
    const bool hard_overflow =
        (capacity_ != 0 && entries_.size() > capacity_ * kHardOverflowFactor) ||
        (byte_budget_ != 0 && bytes_ > byte_budget_ * kHardOverflowFactor);

    while (true) {
        const bool over_count  = (capacity_ != 0) && (entries_.size() > capacity_);
        const bool over_budget = (byte_budget_ != 0) && (bytes_ > byte_budget_);
        if (!over_count && !over_budget) {
            return;
        }

        // 从「最久未使用」一端往回找第一个**不是本帧用过**的条目当牺牲者。
        // 本帧用过的（stamp == frame_）一律跳过 —— 这正是修复 100% 未命中的关键。
        std::string victim;
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            const auto found = entries_.find(*it);
            if (found == entries_.end()) {
                continue;
            }
            if (!hard_overflow && found->second.stamp == frame_) {
                continue;
            }
            victim = *it;
            break;
        }
        if (victim.empty()) {
            return; // 全是本帧刚用过的 → 本帧不再淘汰（下一帧它们自然变旧；硬上限兜底）
        }
        eraseEntry(victim);
        ++evictions_;
    }
}


// ---------------------------------------------------------------------------
//  取纹理：键命中即返回；未命中 → 嗅探格式 + 解码 + 上传 → 入缓存（失败也缓存）
// ---------------------------------------------------------------------------
TextureInfo TextureCache::textureFor(const std::string& path)
{
    if (path.empty()) {
        TextureInfo empty;
        empty.error = "图片路径为空";
        return empty;
    }

    ++requests_;
    const std::string key = cacheKey(path);
    if (const auto found = entries_.find(key); found != entries_.end()) {
        ++hits_;
        touch(key);
        return found->second.info;
    }
    ++misses_;

    TextureInfo info;
    // P7a-16：顺手按内容嗅探格式（只读文件头）→ 供节点卡片 / 输出面板显示**格式徽标**
    if (const utils::ImageMagic magic = utils::sniffImage(path);
        magic.format != utils::ImageFormat::Unknown) {
        info.format = utils::imageFormatName(magic.format);
    }
    // M7-04：解码统一走 utils::image_decode（stb 优先 / WIC 兜底；错误文案可直接显示）
    ++decodes_;
    const utils::DecodedImage decoded = utils::decodeImageRgba8(path);
    if (!decoded.error.empty()) {
        info.error = decoded.error;
        log::error("[纹理缓存] " + info.error);
    }
    else {
        GLint max_texture_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
        if (max_texture_size > 0 &&
            (decoded.width > max_texture_size || decoded.height > max_texture_size)) {
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
            ++uploads_;
            info.texture = static_cast<unsigned int>(texture);
            info.width   = decoded.width;
            info.height  = decoded.height;
        }
    }

    order_.push_front(key);
    const std::size_t bytes =
        (info.texture != 0)
            ? (static_cast<std::size_t>(info.width) * static_cast<std::size_t>(info.height) * 4u)
            : 0u;
    bytes_ += bytes;
    entries_[key] = Entry{info, order_.begin(), bytes, frame_};
    evictIfNeeded();
    return info;
}

bool imageSize(const std::string& path, int* width, int* height, std::string* error)
{
    // M7-04：实现搬到 utils::image_decode（stb 优先 / WIC 兜底 + 可操作文案）
    return utils::readImageSize(path, width, height, error);
}

void TextureCache::beginFrame()
{
    // 逐帧标记（契约：每帧渲染开头调用一次，见 ui/app.cpp）
    ++frame_;
}

void TextureCache::setCapacity(std::size_t capacity)
{
    capacity_ = capacity;
    evictIfNeeded();
}

void TextureCache::setByteBudget(std::size_t bytes)
{
    byte_budget_ = bytes;
    evictIfNeeded();
}

TextureCacheStats TextureCache::stats() const
{
    TextureCacheStats result;
    result.requests    = requests_;
    result.hits        = hits_;
    result.misses      = misses_;
    result.decodes     = decodes_;
    result.uploads     = uploads_;
    result.evictions   = evictions_;
    result.entries     = entries_.size();
    result.bytes       = bytes_;
    result.capacity    = capacity_;
    result.byte_budget = byte_budget_;
    return result;
}

void TextureCache::releaseAll()
{
    for (auto& item : entries_) {
        releaseEntry(item.second);
    }
    entries_.clear();
    order_.clear();
    bytes_ = 0;
}

} // namespace aiwrite::ui
