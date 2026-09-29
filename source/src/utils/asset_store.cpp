#include "utils/asset_store.h"

#include "utils/crypto.h"        // SHA3-256（复用既有实现；无需新增依赖）
#include "utils/image_decode.h"  // 内容嗅探 → 规范扩展名
#include "utils/log.h"
#include "utils/paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace aiwrite::asset {
namespace {

constexpr const char* kTokenPrefixLiteral = "aiwrite-asset:";
constexpr std::size_t kMaxDigestBytes     = 128u * 1024u * 1024u; // 归档上限（防超大文件 OOM）

bool is_hex_digest(const std::string& text)
{
    if (text.size() != 64) {
        return false;
    }
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

} // namespace

std::filesystem::path images_root()
{
    return paths::assets_images_dir();
}

bool is_token(const std::string& value)
{
    const std::string prefix(kTokenPrefixLiteral);
    if (value.rfind(prefix, 0) != 0) {
        return false;
    }
    return is_hex_digest(value.substr(prefix.size()));
}

std::string token_digest(const std::string& value)
{
    if (!is_token(value)) {
        return {};
    }
    return lowercase(value.substr(std::string(kTokenPrefixLiteral).size()));
}

std::string make_token(const std::string& digest)
{
    if (!is_hex_digest(digest)) {
        return {};
    }
    return std::string(kTokenPrefixLiteral) + lowercase(digest);
}

std::string digest_of_file(const std::string& path, std::string* error)
{
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return std::string();
    };
    if (path.empty()) {
        return fail("图片路径为空：无法归档");
    }
    const std::filesystem::path file(path);
    std::error_code             code;
    if (!std::filesystem::exists(file, code) || !std::filesystem::is_regular_file(file, code)) {
        return fail("图片文件不存在（**路径**类错误）: " + path);
    }
    const auto size = static_cast<std::size_t>(std::filesystem::file_size(file, code));
    if (code) {
        return fail("无法读取图片大小（**路径**类错误）: " + path);
    }
    if (size == 0) {
        return fail("图片文件为空（**格式**类错误）: " + path);
    }
    if (size > kMaxDigestBytes) {
        return fail("图片过大，无法归档（超过 128 MB）: " + path);
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return fail("无法打开图片文件（**路径**类错误）: " + path);
    }
    std::string bytes(size, '\0');
    stream.read(&bytes[0], static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) {
        return fail("读取图片文件不完整（**格式**类错误）: " + path);
    }
    return crypto::sha3_256(bytes);
}

std::string canonical_extension(const std::string& path)
{
    using utils::ImageFormat;
    switch (utils::sniffImage(path).format) {
    case ImageFormat::Png:    return ".png";
    case ImageFormat::Jpeg:   return ".jpg";
    case ImageFormat::Gif:    return ".gif";
    case ImageFormat::Bmp:    return ".bmp";
    case ImageFormat::WebP:   return ".webp";
    case ImageFormat::Tiff:   return ".tif";
    case ImageFormat::Ico:    return ".ico";
    case ImageFormat::JpegXr: return ".jxr";
    case ImageFormat::Heif:   return ".heif";
    case ImageFormat::Avif:   return ".avif";
    case ImageFormat::Psd:    return ".psd";
    case ImageFormat::Hdr:    return ".hdr";
    case ImageFormat::Pnm:    return ".pnm";
    case ImageFormat::Unknown: break;
    }
    // 认不出来 → 回退原扩展名（小写）；无扩展名 → `.png`（与 M7-05「未知扩展名仍是 image/png」一致）
    const std::string ext = lowercase(std::filesystem::path(path).extension().string());
    if (ext.size() >= 2 && ext.size() <= 8 && ext[0] == '.') {
        return ext;
    }
    return ".png";
}

std::string import_file(const std::string& path, std::string* error)
{
    const std::string digest = digest_of_file(path, error);
    if (digest.empty()) {
        return {};
    }

    const std::filesystem::path root = images_root();
    std::error_code             code;
    std::filesystem::create_directories(root, code);
    if (code) {
        if (error != nullptr) {
            *error = "无法创建资源目录: " + root.string() + "（" + code.message() + "）";
        }
        return {};
    }

    const std::filesystem::path dest = root / (digest + canonical_extension(path));
    if (!std::filesystem::exists(dest, code)) {
        std::error_code copy_code;
        std::filesystem::copy_file(path, dest, std::filesystem::copy_options::none, copy_code);
        std::error_code check_code;
        if (!std::filesystem::exists(dest, check_code)) {
            if (error != nullptr) {
                *error = "复制进资源目录失败: " + path + " → " + dest.string() +
                         "（" + copy_code.message() + "）";
            }
            return {};
        }
    }
    log::info("[资源目录] 已归档: " + path + " → " + dest.string());
    return make_token(digest);
}

std::string resolve_token(const std::string& token, bool* missing, std::string* error)
{
    const auto mark_missing = [missing, error](const std::string& message) {
        if (missing != nullptr) {
            *missing = true;
        }
        if (error != nullptr) {
            *error = message;
        }
        return std::string();
    };

    if (missing != nullptr) {
        *missing = false;
    }

    const std::string           digest = token_digest(token);
    const std::filesystem::path root   = images_root();
    if (digest.empty()) {
        return mark_missing("不是合法的资源令牌（应为 " + std::string(kTokenPrefixLiteral) +
                            " + 64 位十六进制摘要）: " + token);
    }

    std::error_code code;
    if (std::filesystem::exists(root, code)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(root, code)) {
            if (!entry.is_regular_file(code)) {
                continue;
            }
            if (lowercase(entry.path().stem().string()) != digest) {
                continue;
            }
            return entry.path().string(); // 文件名即摘要 → 不再逐次校验内容（P7a-04）
        }
    }

    return mark_missing("图片资源缺失（可能已被删除）：在资源目录 " + root.string() + " 中找不到 " +
                        token + "（预期文件 " + digest +
                        ".*）→ 请重新选择图片；若图片还在本机，可点「迁移到资源目录」重新归档（P7a-07）");
}

std::string to_local_path(const std::string& value, bool* missing, std::string* error)
{
    if (missing != nullptr) {
        *missing = false;
    }
    if (!is_token(value)) {
        return value; // 旧绝对路径 / 空值：原样返回（不变量 I19）
    }
    return resolve_token(value, missing, error);
}

std::string to_local_path(const std::string& value)
{
    return to_local_path(value, nullptr, nullptr);
}

std::vector<std::string> to_local_paths(const std::vector<std::string>& values,
                                        std::vector<std::string>*     problems)
{
    std::vector<std::string> resolved;
    resolved.reserve(values.size());
    for (const std::string& value : values) {
        if (value.empty()) {
            continue;
        }
        bool             missing = false;
        std::string      error;
        const std::string local = to_local_path(value, &missing, &error);
        if (missing || local.empty()) {
            if (problems != nullptr) {
                problems->push_back(error.empty() ? ("图片不可用: " + value) : error);
            }
            continue;
        }
        resolved.push_back(local);
    }
    return resolved;
}

bool needs_migration(const std::string& value)
{
    return !value.empty() && !is_token(value);
}

std::string migration_hint(const std::string& value)
{
    return "提示：该图片仍是外部路径（" + value + "）——建议点参数面板的「迁移到资源目录」把它复制进 " +
           images_root().string() + "；之后工作流换目录 / 换机仍能取到图片（P7a-06）";
}

} // namespace aiwrite::asset