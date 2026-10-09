#include "utils/image_decode.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>   // CoInitializeEx
#include <wincodec.h>  // IWICImagingFactory（系统图片解码器）
#endif

// stb_image：编译期实现（全工程只有本文件 define STB_IMAGE_IMPLEMENTATION）
// STBI_WINDOWS_UTF8：路径按 UTF-8 转宽字符 + _wfopen —— 中文目录/文件名不再打不开
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4189 4244 4245 4456 4457 4701 4702 4703 4996)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace aiwrite::utils {
namespace {

std::string lower_text(const std::string& text)
{
    std::string out = text;
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

// UTF-8 串 → 宽字符（Windows 上 std::filesystem::path 才不会按 ACP 误读）
std::wstring utf8_to_wide(const std::string& text)
{
#if defined(_WIN32)
    if (text.empty()) {
        return std::wstring();
    }
    const int size =
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
    return result;
#else
    return std::wstring(text.begin(), text.end());
#endif
}

std::string wide_to_utf8(const std::wstring& text)
{
#if defined(_WIN32)
    if (text.empty()) {
        return std::string();
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size,
                          nullptr, nullptr);
    return result;
#else
    return std::string(text.begin(), text.end());
#endif
}

// 用宽字符路径构造（Windows 下 std::filesystem / fstream 都走 _wopen 族）
std::filesystem::path path_from_utf8(const std::string& text)
{
#if defined(_WIN32)
    const std::wstring wide = utf8_to_wide(text);
    if (wide.empty()) {
        return std::filesystem::path();
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(text);
#endif
}

// 读文件头（最多 want 字节）；返回是否读到内容
bool read_head(const std::string& path, unsigned char* out, std::size_t want, std::size_t* got)
{
    std::ifstream stream(path_from_utf8(path), std::ios::binary);
    if (!stream) {
        *got = 0;
        return false;
    }
    stream.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(want));
    *got = static_cast<std::size_t>(stream.gcount());
    return *got > 0;
}

std::string hex_head(const unsigned char* data, std::size_t size, std::size_t limit)
{
    static const char* digits = "0123456789ABCDEF";
    std::string        out;
    const std::size_t  count = size < limit ? size : limit;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out += ' ';
        }
        out += digits[(data[i] >> 4) & 0x0F];
        out += digits[data[i] & 0x0F];
    }
    return out;
}

std::string ext_label_of(const std::string& path)
{
    return lower_text(path_from_utf8(path).extension().string());
}

ImageFormat detect_format(const unsigned char* data, std::size_t size)
{
    const auto has = [&](std::size_t offset, const char* signature, std::size_t length) {
        return size >= offset + length && std::memcmp(data + offset, signature, length) == 0;
    };
    const auto brand_is = [&](const char* brand) { return has(8, brand, 4); };

    if (has(0, "\x89PNG\r\n\x1a\n", 8)) {
        return ImageFormat::Png;
    }
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return ImageFormat::Jpeg;
    }
    if (has(0, "GIF87a", 6) || has(0, "GIF89a", 6)) {
        return ImageFormat::Gif;
    }
    if (has(0, "BM", 2)) {
        return ImageFormat::Bmp;
    }
    if (has(0, "RIFF", 4) && has(8, "WEBP", 4)) {
        return ImageFormat::WebP;
    }
    if (has(0, "II*\0", 4) || has(0, "MM\0*", 4)) {
        return ImageFormat::Tiff;
    }
    if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x00) {
        return ImageFormat::Ico; // ICO / CUR 共用文件头
    }
    if (has(0, "II\xBC\x01", 4) || has(0, "MM\x00\xBC", 4)) {
        return ImageFormat::JpegXr; // HD Photo / JPEG-XR
    }
    if (has(0, "8BPS", 4)) {
        return ImageFormat::Psd;
    }
    if (has(0, "#?RADIANCE", 10) || has(0, "#?RGBE", 6)) {
        return ImageFormat::Hdr;
    }
    if (size >= 2 && data[0] == 'P' && data[1] >= '1' && data[1] <= '6') {
        return ImageFormat::Pnm;
    }
    if (has(4, "ftyp", 4)) { // ISO-BMFF 容器：品牌决定 HEIF / AVIF
        if (brand_is("heic") || brand_is("heix") || brand_is("hevc") || brand_is("hevx") ||
            brand_is("mif1") || brand_is("msf1")) {
            return ImageFormat::Heif;
        }
        if (brand_is("avif") || brand_is("avis")) {
            return ImageFormat::Avif;
        }
    }
    return ImageFormat::Unknown;
}

// stb 覆盖的格式（Unknown 也交给 stb 先试：stb 自己按内容嗅探）
bool stb_covers(ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:
    case ImageFormat::Jpeg:
    case ImageFormat::Gif:
    case ImageFormat::Bmp:
    case ImageFormat::Psd:
    case ImageFormat::Hdr:
    case ImageFormat::Pnm:
    case ImageFormat::Unknown:
        return true;
    default:
        return false;
    }
}

bool needs_system_decoder(ImageFormat format)
{
    switch (format) {
    case ImageFormat::WebP:
    case ImageFormat::Heif:
    case ImageFormat::Avif:
    case ImageFormat::JpegXr:
        return true;
    default:
        return false;
    }
}

#if defined(_WIN32)

// ============================================================================
//  ⚠️ 冻结区（FROZEN · M7-D7 · 2026-09-27）—— 改动前先读 source/README.md §图片解码
//  事故：曾用「合成 30 字节 WebP 头」喂 WIC 探测能力 → 本机装 WebpImageExtension 时
//        解码器解析截断比特流 → CRT abort（退出码 3 / 0x80000003），api_probe 与
//        GUI 参数面板都会踩到。
//  规则（不变量 I17）：**绝不把合成 / 截断 / 未识别格式的数据交给 WIC**
//        · 能力探测只用「解码器元数据枚举」（不解析比特流）
//        · 真正的解码只在「内容已嗅探为 WIC 类格式」时调用
// ============================================================================

// COM 作用域：RPC_E_CHANGED_MODE 表示本线程已按其它模式初始化（复用即可，不配对卸载）
struct ComScope {
    bool uninit = false;
    ComScope()
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninit           = (hr == S_OK || hr == S_FALSE);
    }
    ~ComScope()
    {
        if (uninit) {
            ::CoUninitialize();
        }
    }
    ComScope(const ComScope&)            = delete;
    ComScope& operator=(const ComScope&) = delete;
};

IWICImagingFactory* create_factory()
{
    IWICImagingFactory* factory = nullptr;
    const HRESULT       hr      = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                                     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return SUCCEEDED(hr) ? factory : nullptr;
}

bool wic_decode(const std::string& path, std::vector<unsigned char>* out, int* width, int* height,
                std::string* reason)
{
    ComScope            com;
    IWICImagingFactory* factory = create_factory();
    if (factory == nullptr) {
        *reason = "Windows 图像组件（WIC）不可用";
        return false;
    }

    bool                   ok      = false;
    IWICBitmapDecoder*     decoder = nullptr;
    IWICBitmapFrameDecode* frame   = nullptr;
    IWICFormatConverter*   convert = nullptr;

    const std::wstring wide = utf8_to_wide(path);
    HRESULT            hr   = factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
                                                                 WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr) && decoder != nullptr) {
        hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr) && frame != nullptr) {
        hr = factory->CreateFormatConverter(&convert);
    }
    if (SUCCEEDED(hr) && frame != nullptr && convert != nullptr &&
        SUCCEEDED(convert->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                      nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0;
        UINT h = 0;
        if (SUCCEEDED(convert->GetSize(&w, &h)) && w > 0 && h > 0) {
            const UINT                 stride = w * 4u;
            const UINT                 bytes  = stride * h;
            std::vector<unsigned char> buffer(static_cast<std::size_t>(bytes));
            if (SUCCEEDED(convert->CopyPixels(nullptr, stride, bytes, buffer.data()))) {
                *out    = std::move(buffer);
                *width  = static_cast<int>(w);
                *height = static_cast<int>(h);
                ok      = true;
            }
            else {
                *reason = "WIC 取像素失败";
            }
        }
        else {
            *reason = "WIC 读不到尺寸";
        }
    }
    else if (SUCCEEDED(hr)) {
        *reason = "WIC 无法转换像素格式（可能缺对应解码器）";
    }

    if (convert != nullptr) {
        convert->Release();
    }
    if (frame != nullptr) {
        frame->Release();
    }
    if (decoder != nullptr) {
        decoder->Release();
    }
    factory->Release();
    if (!ok && reason->empty()) {
        *reason = "WIC 没有可用解码器（本机未注册该格式）";
    }
    return ok;
}

bool wic_size(const std::string& path, int* width, int* height, std::string* reason)
{
    ComScope            com;
    IWICImagingFactory* factory = create_factory();
    if (factory == nullptr) {
        *reason = "Windows 图像组件（WIC）不可用";
        return false;
    }

    bool                   ok      = false;
    IWICBitmapDecoder*     decoder = nullptr;
    IWICBitmapFrameDecode* frame   = nullptr;

    const std::wstring wide = utf8_to_wide(path);
    HRESULT            hr   = factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
                                                                 WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr) && decoder != nullptr) {
        hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr) && frame != nullptr) {
        UINT w = 0;
        UINT h = 0;
        if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0) {
            *width  = static_cast<int>(w);
            *height = static_cast<int>(h);
            ok      = true;
        }
        else {
            *reason = "WIC 读不到尺寸";
        }
    }
    if (!ok && reason->empty()) {
        *reason = "WIC 没有可用解码器（本机未注册该格式）";
    }

    if (frame != nullptr) {
        frame->Release();
    }
    if (decoder != nullptr) {
        decoder->Release();
    }
    factory->Release();
    return ok;
}

// 系统是否注册了某个解码器：**只读「解码器元数据枚举」，不解析任何比特流**
// （冻结区规则 I17：合成 / 截断数据喂解码器会让 CRT abort —— 见文件头横幅）
// 注意：SDK 头里枚举器是标准 IEnumUnknown（IWICComponentEnumerator 未在 C++ 头暴露），
//       每项再 QI 成 IWICComponentInfo 读友好名。
bool wic_has_decoder_named(const char* lower_needle)
{
    ComScope            com;
    IWICImagingFactory* factory = create_factory();
    if (factory == nullptr) {
        return false;
    }

    bool         found      = false;
    IEnumUnknown* enumerator = nullptr;
    if (SUCCEEDED(factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault,
                                                     &enumerator)) &&
        enumerator != nullptr) {
        IUnknown* unknown = nullptr;
        ULONG     fetched = 0;
        while (!found && enumerator->Next(1, &unknown, &fetched) == S_OK && fetched == 1 &&
               unknown != nullptr) {
            IWICComponentInfo* info = nullptr;
            if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&info))) && info != nullptr) {
                UINT length = 0;
                if (SUCCEEDED(info->GetFriendlyName(0, nullptr, &length)) && length > 0) {
                    std::wstring name(static_cast<std::size_t>(length), L'\0');
                    if (SUCCEEDED(info->GetFriendlyName(length, name.data(), &length))) {
                        found =
                            lower_text(wide_to_utf8(name)).find(lower_needle) != std::string::npos;
                    }
                }
                info->Release();
            }
            unknown->Release();
            unknown = nullptr;
        }
        enumerator->Release();
    }
    factory->Release();
    return found;
}

#endif // _WIN32

} // namespace

const char* imageFormatName(ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:    return "PNG";
    case ImageFormat::Jpeg:   return "JPEG";
    case ImageFormat::Gif:    return "GIF";
    case ImageFormat::Bmp:    return "BMP";
    case ImageFormat::WebP:   return "WebP";
    case ImageFormat::Tiff:   return "TIFF";
    case ImageFormat::Ico:    return "ICO";
    case ImageFormat::JpegXr: return "JPEG-XR";
    case ImageFormat::Heif:   return "HEIF/HEIC";
    case ImageFormat::Avif:   return "AVIF";
    case ImageFormat::Psd:    return "PSD";
    case ImageFormat::Hdr:    return "HDR";
    case ImageFormat::Pnm:    return "PNM";
    default:                  return "未知格式";
    }
}

const char* imageFormatMime(ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:    return "image/png";
    case ImageFormat::Jpeg:   return "image/jpeg";
    case ImageFormat::Gif:    return "image/gif";
    case ImageFormat::Bmp:    return "image/bmp";
    case ImageFormat::WebP:   return "image/webp";
    case ImageFormat::Tiff:   return "image/tiff";
    case ImageFormat::Ico:    return "image/x-icon";
    case ImageFormat::JpegXr: return "image/jxr";
    case ImageFormat::Heif:   return "image/heic";
    case ImageFormat::Avif:   return "image/avif";
    case ImageFormat::Psd:    return "image/vnd.adobe.photoshop";
    case ImageFormat::Hdr:    return "image/vnd.radiance";
    case ImageFormat::Pnm:    return "image/x-portable-anymap";
    default:                  return "";
    }
}

ImageMagic sniffImageBytes(const unsigned char* data, std::size_t size)
{
    ImageMagic magic;
    if (data != nullptr && size > 0) {
        magic.format = detect_format(data, size);
    }
    return magic;
}

ImageMagic sniffImage(const std::string& path)
{
    unsigned char head[32] = {};
    std::size_t   got      = 0;
    const bool    read_ok  = read_head(path, head, sizeof(head), &got);

    ImageMagic magic = sniffImageBytes(read_ok ? head : nullptr, read_ok ? got : 0);
    magic.ext_label  = ext_label_of(path);
    magic.ext_mime   = mimeFromExtension(path);
    if (read_ok) {
        magic.head_hex = hex_head(head, got, 8);
    }

    const char* content_mime = imageFormatMime(magic.format);
    if (magic.format != ImageFormat::Unknown && content_mime[0] != '\0' && !magic.ext_label.empty() &&
        magic.ext_mime != content_mime) {
        magic.mismatch = true;
    }
    return magic;
}

std::string mimeFromExtension(const std::string& path)
{
    const std::string extension = ext_label_of(path);
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    if (extension == ".bmp") {
        return "image/bmp";
    }
    if (extension == ".gif") {
        return "image/gif";
    }
    if (extension == ".tif" || extension == ".tiff") {
        return "image/tiff";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    if (extension == ".heic" || extension == ".heif") {
        return "image/heic";
    }
    if (extension == ".avif") {
        return "image/avif";
    }
    return "image/png"; // 未知扩展名回退（服务端多按内容嗅探）
}

WicInfo wicInfo()
{
    static const WicInfo cached = [] {
        WicInfo info;
#if defined(_WIN32)
        ComScope            com;
        IWICImagingFactory* factory = create_factory();
        info.available              = (factory != nullptr);
        if (factory != nullptr) {
            factory->Release();
            // 冻结区规则 I17：只用「解码器元数据枚举」探测，**不喂任何合成/截断数据**
            info.webp = wic_has_decoder_named("webp");
            info.heif = wic_has_decoder_named("heif") || wic_has_decoder_named("heic");
            info.avif = wic_has_decoder_named("avif");
        }
        info.note = info.available ? std::string("Windows 图像组件可用")
                                   : std::string("Windows 图像组件（WIC）不可用");
        if (info.available) {
            info.note += info.webp ? "；系统已注册 WebP 解码器" : "；系统未注册 WebP 解码器";
        }
#else
        info.note = "非 Windows 平台：仅 stb_image 可用";
#endif
        return info;
    }();
    return cached;
}

std::string decodeErrorMessage(const std::string& path, const ImageMagic& magic,
                               const std::string& reason)
{
    std::string message = path;
    if (magic.format == ImageFormat::Unknown) {
        message += "：无法识别的图片格式";
        if (!magic.ext_label.empty()) {
            message += "（扩展名 " + magic.ext_label + "）";
        }
        message += "。请确认文件未损坏，或另存为 PNG / JPG 后重试"
                   "（本地支持：PNG、JPEG、BMP、GIF、TGA、PSD、HDR、PNM + Windows 系统解码器提供的 "
                   "WebP / TIFF / ICO / HEIF 等）";
        if (!magic.head_hex.empty()) {
            message += "；文件头：" + magic.head_hex;
        }
    }
    else {
        const std::string label = imageFormatName(magic.format);
        if (magic.mismatch) {
            message += "：文件内容其实是 " + label + "，但扩展名是 " + magic.ext_label +
                       "（扩展名与实际格式不一致）";
        }
        else {
            message += "：文件内容是 " + label;
        }
        message += "，本地预览读取失败";
        if (needs_system_decoder(magic.format)) {
            // 文案**不依赖**能力探测结果（探测只影响括号里的说明），保证离线断言与
            // 跨机器行为一致（冻结区规则 I17 的附带约定）
            const WicInfo wic = wicInfo();
            message += "：该格式依赖 Windows 系统解码器";
            message += wic.available ? "（WIC 可用" : "（WIC 不可用";
            message += (magic.format == ImageFormat::WebP && wic.webp)
                           ? "，已注册 WebP 解码器）"
                           : "，未检测到该格式解码器）";
            message += "；可安装 Windows「Webp / HEIF 图像扩展」等系统扩展后重试，"
                       "或用画图 / 截图工具另存为 PNG / JPG";
        }
        message += "。注意：本地预览失败不影响「图片理解」—— 该节点按文件内容识别格式后发给模型，可照常运行";
    }
    if (!reason.empty()) {
        message += "（解码器原因：" + reason + "）";
    }
    return message;
}

DecodedImage decodeImageRgba8(const std::string& path)
{
    DecodedImage out;
    if (path.empty()) {
        out.error = "图片路径为空：请在「图片输入」节点点「浏览…」选择一张图片";
        return out;
    }

    std::error_code             code;
    const std::filesystem::path file = path_from_utf8(path);
    if (!std::filesystem::exists(file, code) || std::filesystem::is_directory(file, code)) {
        out.error = "图片文件不存在：" + path;
        return out;
    }
    const auto file_size = std::filesystem::file_size(file, code);
    if (code) {
        out.error = "无法读取图片大小：" + path;
        return out;
    }
    if (file_size == 0) {
        out.error = "图片文件为空：" + path;
        return out;
    }

    const ImageMagic         magic = sniffImage(path);
    std::vector<std::string> reasons;

    const auto try_stb = [&]() -> bool {
        int            width    = 0;
        int            height   = 0;
        int            channels = 0;
        unsigned char* pixels   = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr) {
            const char* reason = stbi_failure_reason();
            reasons.push_back(std::string("stb：") + (reason != nullptr ? reason : "未知原因"));
            return false;
        }
        out.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                              static_cast<std::size_t>(height) * 4u);
        stbi_image_free(pixels);
        out.width   = width;
        out.height  = height;
        out.decoder = "stb";
        return true;
    };
    const auto try_wic = [&]() -> bool {
#if defined(_WIN32)
        std::string reason;
        if (wic_decode(path, &out.rgba, &out.width, &out.height, &reason)) {
            out.decoder = "wic";
            return true;
        }
        reasons.push_back("WIC：" + reason);
        return false;
#else
        reasons.push_back("WIC：非 Windows 平台不可用");
        return false;
#endif
    };

    bool ok = false;
    // 冻结区规则 I17：只有「内容已嗅探为 WIC 类格式」（WebP/TIFF/ICO/JXR/HEIF/AVIF）
    // 才把文件交给系统解码器；未知格式 / stb 覆盖的格式**绝不**交给 WIC（避免解析可疑数据）
    if (stb_covers(magic.format)) { // PNG/JPEG/BMP/GIF/PSD/HDR/PNM 或未识别 → stb 自己嗅探
        ok = try_stb();
    }
    else {
        ok = try_wic();
        if (!ok) {
            ok = try_stb();
        }
    }

    if (!ok) {
        std::string joined;
        for (const std::string& item : reasons) {
            joined += joined.empty() ? item : "；" + item;
        }
        out.rgba.clear();
        out.width  = 0;
        out.height = 0;
        out.error  = decodeErrorMessage(path, magic, joined);
    }
    return out;
}

bool readImageSize(const std::string& path, int* width, int* height, std::string* error)
{
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (path.empty()) {
        return fail("图片路径为空：请在「图片输入」节点点「浏览…」选择一张图片");
    }

    std::error_code             code;
    const std::filesystem::path file = path_from_utf8(path);
    if (!std::filesystem::exists(file, code) || std::filesystem::is_directory(file, code)) {
        return fail("图片文件不存在：" + path);
    }
    if (std::filesystem::file_size(file, code) == 0 || code) {
        return fail("图片文件为空：" + path);
    }

    const ImageMagic         magic = sniffImage(path);
    std::vector<std::string> reasons;
    int                      found_width  = 0;
    int                      found_height = 0;

    const auto try_stb = [&]() -> bool {
        int width_value  = 0;
        int height_value = 0;
        int channels     = 0;
        if (stbi_info(path.c_str(), &width_value, &height_value, &channels)) {
            found_width  = width_value;
            found_height = height_value;
            return true;
        }
        const char* reason = stbi_failure_reason();
        reasons.push_back(std::string("stb：") + (reason != nullptr ? reason : "未知原因"));
        return false;
    };
    const auto try_wic = [&]() -> bool {
#if defined(_WIN32)
        std::string reason;
        if (wic_size(path, &found_width, &found_height, &reason)) {
            return true;
        }
        reasons.push_back("WIC：" + reason);
        return false;
#else
        reasons.push_back("WIC：非 Windows 平台不可用");
        return false;
#endif
    };

    bool ok = false;
    if (stb_covers(magic.format)) { // 冻结区规则 I17：stb 覆盖 / 未识别 → 不碰 WIC
        ok = try_stb();
    }
    else {
        ok = try_wic();
        if (!ok) {
            ok = try_stb();
        }
    }

    if (!ok) {
        std::string joined;
        for (const std::string& item : reasons) {
            joined += joined.empty() ? item : "；" + item;
        }
        return fail(decodeErrorMessage(path, magic, joined));
    }
    if (width != nullptr) {
        *width = found_width;
    }
    if (height != nullptr) {
        *height = found_height;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

} // namespace aiwrite::utils