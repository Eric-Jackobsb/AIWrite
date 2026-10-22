#include "image_preprocess.h"

#include "image_decode.h" // M7-04：嗅探 / 解码（**只调用、不修改** —— 守 I17 冻结区）
#include "image_convert.h" // default_upload_tmp_dir：临时目录口径与上传准备一致

#include <stb_image_resize.h> // stbir_resize_uint8（与 image_convert 同族，零新增依赖）
#include <stb_image_write.h>  // stbi_write_png（写临时 PNG）

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace aiwrite::ai {
namespace {

// 预处理临时文件名（进程内递增 + 时间种子；不要求跨进程稳定）
std::string next_preprocess_file()
{
    static std::atomic<unsigned long long> counter{0};
    return "pp-" + std::to_string(static_cast<unsigned long long>(
                       std::time(nullptr))) +
           "-" + std::to_string(counter.fetch_add(1)) + ".png";
}

} // namespace

PreprocessOutcome preprocess_image(const std::string& local_path,
                                   const ImagePreprocessParams& params)
{
    PreprocessOutcome outcome;
    const bool        need_resize =
        params.resize_mode != "none" && (params.resize_max_width > 0 || params.resize_max_height > 0);
    if (!need_resize && !params.crop_enabled) {
        outcome.ok      = true;
        outcome.path    = local_path;
        outcome.changed = false;
        return outcome; // 零开销：不复制、不解码、不写盘
    }

    const utils::DecodedImage decoded = utils::decodeImageRgba8(local_path);
    if (decoded.rgba.empty()) {
        outcome.error = decoded.error.empty() ? std::string("图片解码失败: ") + local_path
                                              : decoded.error;
        return outcome;
    }
    int                        width  = decoded.width;
    int                        height = decoded.height;
    std::vector<unsigned char> pixels = decoded.rgba;
    std::string                note;

    // ---- 1) 裁剪（RGBA8 矩形拷贝；区域 clamp 到画布内；w/h<=0 = 取右侧 / 下侧剩余）----
    if (params.crop_enabled) {
        const int cx = std::max(0, params.crop_x);
        const int cy = std::max(0, params.crop_y);
        int       cw = params.crop_w > 0 ? params.crop_w : (width - cx);
        int       ch = params.crop_h > 0 ? params.crop_h : (height - cy);
        cw           = std::max(0, std::min(cw, width - cx));
        ch           = std::max(0, std::min(ch, height - cy));
        if (cw == 0 || ch == 0) {
            outcome.error = "裁剪区域超出图片范围（原图 " + std::to_string(width) + "x" +
                            std::to_string(height) + "，裁剪起点 " + std::to_string(params.crop_x) +
                            "," + std::to_string(params.crop_y) + "）";
            return outcome;
        }
        if (cw != width || ch != height) {
            std::vector<unsigned char> cropped(
                static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch) * 4);
            for (int y = 0; y < ch; ++y) {
                const unsigned char* src =
                    pixels.data() + static_cast<std::size_t>(cy + y) * static_cast<std::size_t>(width) * 4 +
                    static_cast<std::size_t>(cx) * 4;
                std::copy(src, src + static_cast<std::size_t>(cw) * 4,
                          cropped.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) * 4);
            }
            pixels = std::move(cropped);
            width  = cw;
            height = ch;
            note += "裁剪 " + std::to_string(cw) + "x" + std::to_string(ch);
        }
    }

    // ---- 2) 缩放（fit = 等比放入 max 框内；fill = 等比覆盖 max 框后居中裁剪）----
    if (need_resize) {
        // 0 = 不限制（用原尺寸当目标，保证比例计算不出现 0 分母）
        const int    max_w   = params.resize_max_width > 0 ? params.resize_max_width : width;
        const int    max_h   = params.resize_max_height > 0 ? params.resize_max_height : height;
        const double scale_w = static_cast<double>(max_w) / width;
        const double scale_h = static_cast<double>(max_h) / height;
        double       scale   = 1.0;
        if (params.resize_mode == "fill") {
            scale = std::max(scale_w, scale_h); // 短边填满 → 长边溢出（随后居中裁剪）
        }
        else { // fit
            scale = std::min(scale_w, scale_h); // 长边放入 → 短边留白
        }
        int out_w = static_cast<int>(width * scale);
        int out_h = static_cast<int>(height * scale);
        out_w     = std::max(1, out_w);
        out_h     = std::max(1, out_h);

        // 仅当尺寸真的变化时才缩放（fit 且原图已 ≤ 框内 → 不缩放）
        if (out_w != width || out_h != height) {
            std::vector<unsigned char> scaled(
                static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4);
            if (!stbir_resize_uint8(pixels.data(), width, height, 0, scaled.data(), out_w, out_h, 0, 4)) {
                outcome.error = "图片缩放失败（stb 内部错误）";
                return outcome;
            }
            pixels = std::move(scaled);
            width  = out_w;
            height = out_h;
            if (!note.empty()) {
                note += " → ";
            }
            note += "缩放 " + std::to_string(out_w) + "x" + std::to_string(out_h);
        }

        // fill：缩放后若超出一边 → 居中裁剪到 max 框
        if (params.resize_mode == "fill" && (width > max_w || height > max_h)) {
            const int cw = std::min(width, max_w);
            const int ch = std::min(height, max_h);
            const int cx = (width - cw) / 2;
            const int cy = (height - ch) / 2;
            std::vector<unsigned char> cropped(
                static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch) * 4);
            for (int y = 0; y < ch; ++y) {
                const unsigned char* src =
                    pixels.data() + static_cast<std::size_t>(cy + y) * static_cast<std::size_t>(width) * 4 +
                    static_cast<std::size_t>(cx) * 4;
                std::copy(src, src + static_cast<std::size_t>(cw) * 4,
                          cropped.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) * 4);
            }
            pixels = std::move(cropped);
            width  = cw;
            height = ch;
            note += "（fill 居中裁剪 " + std::to_string(cw) + "x" + std::to_string(ch) + "）";
        }
    }

    // ---- 3) 写临时 PNG ----
    const std::string dir = defaultUploadTmpDir();
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    const std::string out_path = dir + "/" + next_preprocess_file();
    if (!stbi_write_png(out_path.c_str(), width, height, 4, pixels.data(),
                        static_cast<int>(sizeof(unsigned char)) * width * 4)) {
        outcome.error = "无法写入预处理结果: " + out_path;
        return outcome;
    }
    outcome.ok      = true;
    outcome.path    = out_path;
    outcome.changed = true;
    outcome.note    = note;
    return outcome;
}

// ------------------------------------------------------------------ 自检 ----
namespace {

// 生成纯色测试 PNG（side 边长；返回路径；失败返回空）
std::string make_test_png(const std::string& dir, const std::string& stem, int side,
                          unsigned char r, unsigned char g, unsigned char b)
{
    const std::string path = dir + "/" + stem + ".png";
    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * side + x) * 4;
            pixels[off + 0] = r;
            pixels[off + 1] = g;
            pixels[off + 2] = b;
            pixels[off + 3] = 255;
        }
    }
    if (!stbi_write_png(path.c_str(), side, side, 4, pixels.data(), side * 4)) {
        return {};
    }
    return path;
}

} // namespace

int imagePreprocessSelftest(int* passed_out)
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

    const std::string dir = defaultUploadTmpDir();
    std::error_code   ec;
    std::filesystem::create_directories(dir, ec);
    const std::string src = make_test_png(dir, "preprocess-selftest-src", 40, 200, 40, 40);

    // 1) 零开销路径：无参数变化 → 原样返回
    {
        ImagePreprocessParams no_op;
        const PreprocessOutcome outcome = preprocess_image(src, no_op);
        check(outcome.ok && !outcome.changed && outcome.path == src,
              "预处理零开销：无参数变化时原样返回（不写盘）");
    }
    // 2) 缩放 fit：40x40 → 放入 20x30 框内 = 20x20
    {
        ImagePreprocessParams params;
        params.resize_mode       = "fit";
        params.resize_max_width  = 20;
        params.resize_max_height = 30;
        const PreprocessOutcome outcome = preprocess_image(src, params);
        int width = 0, height = 0;
        std::string error;
        const bool size_ok = outcome.ok && utils::readImageSize(outcome.path, &width, &height, &error);
        check(outcome.ok && size_ok && width == 20 && height == 20,
              "预处理缩放 fit：40x40 → 20x20（长边放入 20x30 框）");
    }
    // 3) 缩放 fill：40x40 → 等比覆盖 20x30 框（放大到 30x30）→ 居中裁剪 20x30（输出 = 目标框）
    {
        ImagePreprocessParams params;
        params.resize_mode       = "fill";
        params.resize_max_width  = 20;
        params.resize_max_height = 30;
        const PreprocessOutcome outcome = preprocess_image(src, params);
        int width = 0, height = 0;
        std::string error;
        const bool size_ok = outcome.ok && utils::readImageSize(outcome.path, &width, &height, &error);
        check(outcome.ok && size_ok && width == 20 && height == 30,
              "预处理缩放 fill：40x40 → 等比覆盖 20x30 框（30x30）→ 居中裁剪 20x30");
    }
    // 4) 裁剪：40x40 → 裁剪 (10,10,20,20) = 20x20
    {
        ImagePreprocessParams params;
        params.crop_enabled = true;
        params.crop_x       = 10;
        params.crop_y       = 10;
        params.crop_w       = 20;
        params.crop_h       = 20;
        const PreprocessOutcome outcome = preprocess_image(src, params);
        int width = 0, height = 0;
        std::string error;
        const bool size_ok = outcome.ok && utils::readImageSize(outcome.path, &width, &height, &error);
        check(outcome.ok && size_ok && width == 20 && height == 20,
              "预处理裁剪：40x40 → (10,10,20,20) = 20x20");
    }
    // 5) 裁剪 + 缩放组合：40x40 → 裁剪 20x20 → fit 到 10x10
    {
        ImagePreprocessParams params;
        params.crop_enabled     = true;
        params.crop_x           = 0;
        params.crop_y           = 0;
        params.crop_w           = 20;
        params.crop_h           = 20;
        params.resize_mode      = "fit";
        params.resize_max_width = 10;
        const PreprocessOutcome outcome = preprocess_image(src, params);
        int width = 0, height = 0;
        std::string error;
        const bool size_ok = outcome.ok && utils::readImageSize(outcome.path, &width, &height, &error);
        check(outcome.ok && size_ok && width == 10 && height == 10,
              "预处理组合：裁剪 20x20 → 缩放 10x10");
    }
    // 6) 缺文件 → 可操作错误
    {
        ImagePreprocessParams params;
        params.resize_mode       = "fit";
        params.resize_max_width  = 10;
        const PreprocessOutcome outcome =
            preprocess_image(dir + "/no-such-file-pp.png", params);
        check(!outcome.ok && !outcome.error.empty(), "预处理失败路径：缺文件给可操作错误");
    }
    // 清理测试文件
    {
        std::error_code remove_error;
        std::filesystem::remove(src, remove_error);
    }

    std::printf("  预处理自检小计：%d 通过 / %d 失败\n", passed, failed);
    if (passed_out != nullptr) {
        *passed_out = passed;
    }
    return failed;
}

} // namespace aiwrite::ai

