#include "ai/upload/image_convert.h"

// ============================================================================
//  实现说明（与 image_convert.h 头注释配套）
//  * 依赖来源：`stb_image_write.h` / `stb_image_resize.h` 随 vcpkg 的 stb 端口已在树里
//    （与 `stb_image.h` 同目录）→ **零新增依赖**；两个 `*_IMPLEMENTATION` 只在本 TU 定义
//    （`stb_image.h` 的实现仍在 `utils/image_decode.cpp` —— 本模块**不改冻结区**）
//  * 转换只在必要时发生：白名单不含 / 体积超限 / 长边超限
// ============================================================================

#include "utils/paths.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

namespace aiwrite::ai {
namespace {

namespace fs = std::filesystem;

using utils::ImageFormat;

std::string lower_ascii(std::string text)
{
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool is_space_char(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim_ascii(const std::string& text)
{
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && is_space_char(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && is_space_char(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 我们关心的**图片**扩展名（小写、无点）—— 站点 accept 里混着的文档 / 音频一律丢弃
const std::set<std::string>& image_exts()
{
    static const std::set<std::string> exts = {"png",  "jpg",  "jpeg", "jpe",  "jfif", "webp",
                                               "gif",  "bmp",  "tif",  "tiff", "ico",  "heic",
                                               "heif", "avif", "psd",  "hdr",  "pnm",  "ppm",
                                               "pgm",  "pbm"};
    return exts;
}

// 同族扩展名：站点写 .jpg 而内容是 JPEG / 扩展名是 .jpeg —— 一律互通
bool same_family(const std::string& a, const std::string& b)
{
    static const std::set<std::string> jpeg_family = {"jpg", "jpeg", "jpe", "jfif"};
    static const std::set<std::string> tiff_family = {"tif", "tiff"};
    static const std::set<std::string> heif_family = {"heic", "heif"};
    if (a == b) {
        return true;
    }
    const auto both_in = [&a, &b](const std::set<std::string>& family) {
        return family.count(a) > 0 && family.count(b) > 0;
    };
    return both_in(jpeg_family) || both_in(tiff_family) || both_in(heif_family);
}

// ImageFormat → 扩展名（小写无点；Unknown → 空）
std::string ext_of_format(ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:    return "png";
    case ImageFormat::Jpeg:   return "jpeg";
    case ImageFormat::Gif:    return "gif";
    case ImageFormat::Bmp:    return "bmp";
    case ImageFormat::WebP:   return "webp";
    case ImageFormat::Tiff:   return "tiff";
    case ImageFormat::Ico:    return "ico";
    case ImageFormat::JpegXr: return "jxr";
    case ImageFormat::Heif:   return "heic";
    case ImageFormat::Avif:   return "avif";
    case ImageFormat::Psd:    return "psd";
    case ImageFormat::Hdr:    return "hdr";
    case ImageFormat::Pnm:    return "pnm";
    case ImageFormat::Unknown: break;
    }
    return {};
}

// ".PNG" / "image/webp" → "png" / "webp"（去点、去 mime 前缀、小写）
std::string normalize_ext_token(std::string token)
{
    token = trim_ascii(token);
    if (!token.empty() && token[0] == '.') {
        token.erase(0, 1);
    }
    const std::size_t slash = token.find('/');
    if (slash != std::string::npos) {
        token = token.substr(slash + 1);
    }
    return lower_ascii(trim_ascii(token));
}

std::string join_list(const std::vector<std::string>& items)
{
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += " / ";
        }
        out += item;
    }
    return out;
}

std::string human_bytes(std::size_t bytes)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

int max_of(int a, int b)
{
    return a > b ? a : b;
}

// 长边缩放到 max_edge（保比例、不变形；max_edge<=0 或已合规 → 原尺寸）
void fit_edge(int width, int height, int max_edge, int* out_width, int* out_height)
{
    if (max_edge <= 0 || (width <= max_edge && height <= max_edge)) {
        *out_width  = width;
        *out_height = height;
        return;
    }
    const double scale = static_cast<double>(max_edge) / static_cast<double>(max_of(width, height));
    *out_width  = std::max(1, static_cast<int>(static_cast<double>(width) * scale));
    *out_height = std::max(1, static_cast<int>(static_cast<double>(height) * scale));
}

ConvertOutcome fail_outcome(std::string error)
{
    ConvertOutcome outcome;
    outcome.error = std::move(error);
    return outcome;
}

// 目标编码格式（只支持 png / jpg；其它一律回退 png）
std::string encode_extension(const UploadPrepareOptions& options)
{
    std::string ext =
        lower_ascii(options.target_ext.empty() ? std::string("png") : options.target_ext);
    if (ext == "jpeg" || ext == "jpe" || ext == "jfif") {
        ext = "jpg";
    }
    if (ext != "png" && ext != "jpg") {
        ext = "png";
    }
    return ext;
}

// 文件名里不能出现的字符（Windows）→ 下划线
void sanitize_stem(std::string* stem)
{
    for (char& c : *stem) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            c = '_';
        }
    }
}

} // namespace

std::vector<std::string> image_exts_from_accept(const std::string& accept)
{
    std::vector<std::string> out;
    std::string              item;
    for (std::size_t i = 0; i <= accept.size(); ++i) {
        const char current = i < accept.size() ? accept[i] : ',';
        if (current != ',') {
            item.push_back(current);
            continue;
        }
        const std::string token = normalize_ext_token(item);
        item.clear();
        if (token.empty() || image_exts().count(token) == 0) {
            continue; // 非图片（pdf / docx / mp3 …）—— 丢弃
        }
        if (std::find(out.begin(), out.end(), token) == out.end()) {
            out.push_back(token);
        }
    }
    return out;
}

bool accept_allows(const std::vector<std::string>& accept, ImageFormat format,
                   const std::string& ext_label)
{
    if (accept.empty()) {
        return true; // 站点未声明图片格式 = 不限制
    }
    // 内容优先（扩展名可能骗人）；内容认不出来才退回扩展名
    std::string ext = ext_of_format(format);
    if (ext.empty()) {
        ext = normalize_ext_token(ext_label);
    }
    if (ext.empty()) {
        return false; // 内容与扩展名都认不出 → 不放行（宁可如实报错，也不乱传）
    }
    for (const std::string& allowed : accept) {
        if (same_family(allowed, ext)) {
            return true;
        }
    }
    return false;
}

bool need_convert(const utils::ImageMagic& magic, const UploadPrepareOptions& options,
                  std::size_t file_bytes, std::string* reason)
{
    if (!accept_allows(options.accept, magic.format, magic.ext_label)) {
        if (reason != nullptr) {
            const char* name = utils::imageFormatName(magic.format);
            const std::string shown =
                (name != nullptr && *name != '\0')
                    ? std::string(name)
                    : (magic.ext_label.empty() ? std::string("未知格式") : magic.ext_label);
            *reason = shown + " 不在站点白名单内（站点接受：" + join_list(options.accept) + "）";
        }
        return true;
    }
    if (options.max_bytes > 0 && file_bytes > options.max_bytes) {
        if (reason != nullptr) {
            *reason = "体积 " + human_bytes(file_bytes) + " 超过站点上限 " +
                      human_bytes(options.max_bytes);
        }
        return true;
    }
    return false;
}

std::string default_upload_tmp_dir()
{
    std::error_code ec;
    const fs::path  temp = fs::temp_directory_path(ec);
    if (!ec && !temp.empty()) {
        return (temp / "aiwrite-upload").string();
    }
    return (aiwrite::paths::data_root() / "tmp" / "upload").string();
}

ConvertOutcome convert_for_upload(const std::string& local_path, const UploadPrepareOptions& options)
{
    if (local_path.empty()) {
        return fail_outcome("图片值为空（工作流的图片参数没填？请在图片节点选择一张图片）");
    }
    std::error_code ec;
    if (!fs::exists(local_path, ec) || ec) {
        return fail_outcome("图片不存在：" + local_path +
                            "（工作流里的图片值可能已失效 —— 可在图片节点重新选择，或用面板的"
                            "「迁移到资源目录」；资源目录见 ~/.brain-ai/assets/images）");
    }
    if (!fs::is_regular_file(local_path, ec) || ec) {
        return fail_outcome("图片路径不是文件：" + local_path + "（请选择图片文件，而不是目录）");
    }
    const std::uintmax_t size_raw = fs::file_size(local_path, ec);
    if (ec) {
        return fail_outcome("读不到图片大小：" + local_path + "（" + ec.message() + "）");
    }
    const std::size_t file_bytes = static_cast<std::size_t>(size_raw);
    if (file_bytes == 0) {
        return fail_outcome("图片文件是空的（0 字节）：" + local_path +
                            " —— 请重新选择或重新导出该图片");
    }

    const utils::ImageMagic magic = utils::sniffImage(local_path);
    if (magic.format == ImageFormat::Unknown) {
        return fail_outcome("认不出图片格式：" + local_path + "（文件头 = " +
                            (magic.head_hex.empty() ? std::string("(读不到)") : magic.head_hex) +
                            "）—— 支持 PNG/JPEG/WebP/GIF/BMP/TIFF；若确定是图片，"
                            "可在系统里装上对应图片扩展（WebP / HEIF）后重试");
    }

    std::string reason;
    bool        convert = need_convert(magic, options, file_bytes, &reason);

    int  width      = 0;
    int  height     = 0;
    bool size_known = utils::readImageSize(local_path, &width, &height, nullptr);
    if (!size_known) {
        width  = 0;
        height = 0;
    }
    if (options.max_edge > 0 && size_known && max_of(width, height) > options.max_edge) {
        convert = true;
        if (reason.empty()) {
            reason = "长边 " + std::to_string(max_of(width, height)) + "px 超过站点上限 " +
                     std::to_string(options.max_edge) + "px";
        }
    }
    if (!convert) {
        ConvertOutcome outcome;
        outcome.ok   = true;
        outcome.path = local_path; // 零拷贝零开销路径
        return outcome;
    }

    const utils::DecodedImage decoded = utils::decodeImageRgba8(local_path);
    if (decoded.width <= 0 || decoded.height <= 0 || decoded.rgba.empty() ||
        !decoded.error.empty()) {
        return fail_outcome(decoded.error.empty() ? ("解码失败：" + local_path) : decoded.error);
    }

    // 体积超限但站点没给长边上限 → 由「面积 ∝ 字节」反推一个保守长边（一步到位，写完再复核）
    int edge_limit = options.max_edge;
    if (edge_limit <= 0 && options.max_bytes > 0 && file_bytes > options.max_bytes) {
        const double ratio = static_cast<double>(options.max_bytes) /
                             static_cast<double>(file_bytes);
        const double scale      = ratio > 0.0 ? std::sqrt(ratio) * 0.95 : 1.0;
        const int    longest    = max_of(decoded.width, decoded.height);
        edge_limit              = std::max(64, static_cast<int>(static_cast<double>(longest) * scale));
        if (reason.empty()) {
            reason = "体积 " + human_bytes(file_bytes) + " 超过站点上限 " +
                     human_bytes(options.max_bytes) + "（按上限反推长边 " +
                     std::to_string(edge_limit) + "px）";
        }
    }

    int out_width  = decoded.width;
    int out_height = decoded.height;
    fit_edge(decoded.width, decoded.height, edge_limit, &out_width, &out_height);

    std::vector<unsigned char> scaled;
    const unsigned char*       pixels = decoded.rgba.data();
    if (out_width != decoded.width || out_height != decoded.height) {
        scaled.assign(static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height) * 4u,
                      0);
        if (stbir_resize_uint8(pixels, decoded.width, decoded.height, 0, scaled.data(), out_width,
                               out_height, 0, 4) == 0) {
            return fail_outcome("缩放失败（" + std::to_string(decoded.width) + "×" +
                                std::to_string(decoded.height) + " → " +
                                std::to_string(out_width) + "×" + std::to_string(out_height) +
                                "）：" + local_path);
        }
        pixels = scaled.data();
    }

    const std::string ext = encode_extension(options);
    const fs::path    dir = options.tmp_dir.empty() ? fs::path(default_upload_tmp_dir())
                                                    : fs::path(options.tmp_dir);
    std::string       stem = fs::path(local_path).stem().string();
    sanitize_stem(&stem);
    if (stem.empty()) {
        stem = "image";
    }
    const fs::path out_path = dir / (stem + "-aiwrite." + ext);

    std::error_code dir_ec;
    fs::create_directories(dir, dir_ec);
    if (dir_ec) {
        return fail_outcome("创建临时目录失败：" + dir.string() + "（" + dir_ec.message() + "）");
    }

    int written = 0;
    if (ext == "png") {
        written = stbi_write_png(out_path.string().c_str(), out_width, out_height, 4, pixels,
                                 out_width * 4);
    }
    else {
        // stb 的 JPEG 编码器按 3 通道写（RGBA 先降为 RGB）
        const std::size_t pixels_count = static_cast<std::size_t>(out_width) *
                                         static_cast<std::size_t>(out_height);
        std::vector<unsigned char> rgb(pixels_count * 3u, 0);
        for (std::size_t i = 0; i < pixels_count; ++i) {
            rgb[i * 3 + 0] = pixels[i * 4 + 0];
            rgb[i * 3 + 1] = pixels[i * 4 + 1];
            rgb[i * 3 + 2] = pixels[i * 4 + 2];
        }
        written = stbi_write_jpg(out_path.string().c_str(), out_width, out_height, 3, rgb.data(), 90);
    }
    if (written == 0) {
        return fail_outcome("写出转换后的图片失败：" + out_path.string() +
                            "（磁盘空间不足或目录无写权限？）");
    }

    const std::uintmax_t out_bytes = fs::file_size(out_path, ec);
    if (ec || out_bytes == 0) {
        return fail_outcome("转换后的图片为空：" + out_path.string() + "（请重试）");
    }
    if (options.max_bytes > 0 && static_cast<std::size_t>(out_bytes) > options.max_bytes) {
        return fail_outcome("转换后仍超过站点上限 " + human_bytes(options.max_bytes) + "（当前 " +
                            human_bytes(static_cast<std::size_t>(out_bytes)) +
                            "）：请先在工作流里把图片缩小，或另存为 JPEG 后重试（原图：" +
                            local_path + "）");
    }

    ConvertOutcome outcome;
    outcome.ok        = true;
    outcome.path      = out_path.string();
    outcome.converted = true;
    const char* name  = utils::imageFormatName(magic.format);
    outcome.note = std::string(name != nullptr ? name : "图片") + "→" + ext + "（" + reason + "）";
    return outcome;
}

int image_convert_selftest(int* passed_out)
{
    int passed = 0;
    int failed = 0;
    const auto check = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };

    // ① accept 解析：非图片混入 / MIME 形式 / 大小写 / 空项 / 重复
    check(image_exts_from_accept(".png,.PDF, .JPG,.mp3,docx,.jpeg,image/webp,*/*,") ==
              std::vector<std::string>({"png", "jpg", "jpeg", "webp"}),
          "accept 解析：只留图片扩展名（mime / 大写 / 空项 / 非图片混入）");

    // ② 放行判定
    check(accept_allows({}, ImageFormat::WebP, ".webp"), "空白名单 = 不限制");
    check(accept_allows({"jpg"}, ImageFormat::Jpeg, ".jpg"), "白名单 jpg 放行 JPEG 内容");
    check(accept_allows({"jpg"}, ImageFormat::Unknown, ".jpeg"), "扩展名 .jpeg 与白名单 jpg 同族");
    check(!accept_allows({"png"}, ImageFormat::WebP, ".webp"), "白名单 png 拒绝 WebP");
    check(!accept_allows({"png"}, ImageFormat::Unknown, ".xyz"), "内容与扩展名都认不出 → 不放行");

    // ③ 转换判定（纯函数）
    {
        utils::ImageMagic    magic;
        magic.format    = ImageFormat::WebP;
        magic.ext_label = ".webp";
        UploadPrepareOptions options;
        options.accept = {"png"};
        std::string reason;
        check(need_convert(magic, options, 1024, &reason) && !reason.empty(),
              "白名单不含 webp → 需要转换且给出原因");
    }
    {
        utils::ImageMagic    magic;
        magic.format    = ImageFormat::Png;
        magic.ext_label = ".png";
        UploadPrepareOptions options;
        options.accept    = {"png"};
        options.max_bytes = 1024;
        std::string reason;
        check(need_convert(magic, options, 4096, &reason) && !reason.empty(),
              "体积超限 → 需要转换且给出原因");
        std::string quiet;
        check(!need_convert(magic, options, 512, &quiet) && quiet.empty(),
              "合规图片 → 不转换（零开销路径）");
    }

    // ④ 真实转码：临时目录内造图 → 转换 → 复核（离线，无网络、无窗口）
    const fs::path dir        = fs::path(default_upload_tmp_dir()) / "selftest";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path png_path   = dir / "src-64.png";
    const fs::path fake_path  = dir / "fake-content.jpg"; // 内容 PNG、扩展名 .jpg
    const fs::path txt_path   = dir / "not-image.txt";
    const fs::path empty_path = dir / "empty.png";
    const int      side       = 64;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(side) * side * 4u, 0);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * side + x) * 4u;
            pixels[index + 0] = static_cast<unsigned char>(x * 4);
            pixels[index + 1] = static_cast<unsigned char>(y * 4);
            pixels[index + 2] = 128;
            pixels[index + 3] = 255;
        }
    }
    check(stbi_write_png(png_path.string().c_str(), side, side, 4, pixels.data(), side * 4) != 0,
          "自检前置：生成 64×64 PNG 素材");
    {
        std::error_code copy_ec;
        fs::copy_file(png_path, fake_path, fs::copy_options::overwrite_existing, copy_ec);
        check(!copy_ec, "自检前置：复制出「内容 PNG、扩展名 .jpg」的骗人文件");
    }
    {
        std::FILE* file = std::fopen(txt_path.string().c_str(), "wb");
        if (file != nullptr) {
            std::fputs("not an image", file);
            std::fclose(file);
        }
    }
    {
        std::FILE* file = std::fopen(empty_path.string().c_str(), "wb");
        if (file != nullptr) {
            std::fclose(file);
        }
    }

    // 内容优先：白名单 png + 骗人扩展名 .jpg → 放行且不转换
    {
        UploadPrepareOptions options;
        options.accept = {"png"};
        const ConvertOutcome outcome = convert_for_upload(fake_path.string(), options);
        check(outcome.ok && !outcome.converted && outcome.path == fake_path.string(),
              "内容优先：扩展名 .jpg 但内容是 PNG → 白名单 png 放行、不转换");
    }
    // 白名单不含 PNG → 真实解码 + 转码为 png，并记录原因
    {
        UploadPrepareOptions options;
        options.accept = {"jpg"};
        const ConvertOutcome outcome = convert_for_upload(fake_path.string(), options);
        const utils::ImageMagic magic =
            outcome.path.empty() ? utils::ImageMagic() : utils::sniffImage(outcome.path);
        check(outcome.ok && outcome.converted && !outcome.note.empty() &&
                  magic.format == ImageFormat::Png &&
                  outcome.path.find("-aiwrite.png") != std::string::npos,
              "白名单不含 PNG → 真实转码为 png（note 记录原因 / 产物可再嗅探）");
    }
    // 长边超限 → 缩放（保比例）
    {
        UploadPrepareOptions options;
        options.accept   = {"png"};
        options.max_edge = 16;
        const ConvertOutcome outcome = convert_for_upload(png_path.string(), options);
        int width  = 0;
        int height = 0;
        const bool sized =
            outcome.ok && utils::readImageSize(outcome.path, &width, &height, nullptr);
        check(sized && width == 16 && height == 16, "长边超限 → 缩放到 16×16");
    }
    // 体积上限不可满足 → 如实报错（`I21`：不假装成功）
    {
        UploadPrepareOptions options;
        options.accept    = {"png"};
        options.max_bytes = 8;
        const ConvertOutcome outcome = convert_for_upload(png_path.string(), options);
        check(!outcome.ok && outcome.error.find("上限") != std::string::npos,
              "转换后仍超限 → 明确报错（含「上限」）");
    }
    // 失败路径：缺文件 / 非图片 / 空文件
    {
        UploadPrepareOptions options;
        const ConvertOutcome missing = convert_for_upload((dir / "no-such.png").string(), options);
        check(!missing.ok && missing.error.find("不存在") != std::string::npos,
              "缺文件 → 可操作文案（含「不存在」）");
        const ConvertOutcome text = convert_for_upload(txt_path.string(), options);
        check(!text.ok && text.error.find("认不出") != std::string::npos,
              "非图片 → 可操作文案（含「认不出」）");
        const ConvertOutcome empty = convert_for_upload(empty_path.string(), options);
        check(!empty.ok && empty.error.find("空的") != std::string::npos,
              "空文件（0 字节）→ 可操作文案（含「空的」）");
    }
    check(!default_upload_tmp_dir().empty(), "默认临时目录可解析");

    // 清理（best-effort；失败不影响断言）
    std::error_code cleanup_ec;
    fs::remove_all(dir, cleanup_ec);

    if (passed_out != nullptr) {
        *passed_out = passed;
    }
    std::printf("[图片准备自检] 通过 %d / 失败 %d（素材目录 %s，已清理）\n", passed, failed,
                dir.string().c_str());
    return failed;
}





} // namespace aiwrite::ai

