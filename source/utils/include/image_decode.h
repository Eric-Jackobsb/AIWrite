#pragma once

// ============================================================================
//  图片格式嗅探 + 解码（M7-04；决策 M7-D3 = stb 优先 / WIC 兜底）
//
//  ⚠️ **冻结区（FROZEN · M7-D7 · 不变量 I17）**：本文件的「解码分派」与
//     「WIC 能力探测方式」已冻结。改动前先读 source/README.md §4.2。规则：
//       ① 绝不把「合成 / 截断 / 未识别格式」的数据交给 WIC —— 能力探测只允许用
//          「解码器元数据枚举」（IWICComponentInfo::GetFriendlyName），不得用伪造
//          文件头调 CreateDecoderFromStream / CreateDecoderFromFilename 去"试"能力；
//       ② 嗅探 ∈ {PNG,JPEG,BMP,GIF,PSD,HDR,PNM,未知} → 只走 stb；
//          ∈ {WebP,TIFF,ICO,JXR,HEIF,AVIF} → 先 WIC、失败再 stb；
//       ③ 失败文案（decodeErrorMessage）不得依赖能力探测结果（探测细节只进括号说明），
//          否则离线断言会随机器变化而红。
//     事故背景与定位过程：docs/actionPlan/M7.md §1（退出码 3 / 0x80000003）。
//
//  * 嗅探（sniffImage*）：只读文件头魔数，**不整图解码**
//      - 用途①：给模型发对的 MIME（扩展名可能是骗人的，例：WebP 存成 .png）
//      - 用途②：解码失败时给出可操作提示（"内容是 WebP / 缺哪个系统解码器"）
//      - sniffImageBytes 为纯函数版本，供离线断言使用
//  * 解码（decodeImageRgba8）：统一输出 RGBA8、行序自上而下（与 GL 上传一致）
//      - stb_image 覆盖的格式（PNG/JPEG/BMP/GIF/TGA/PSD/HDR/PNM）走 stb，
//        与 M5 起的行为逐像素一致（零色彩回归）
//      - stb 覆盖不了或解不出来的（WebP/TIFF/ICO/HEIF/AVIF/JPEG-XR…）交给
//        Windows WIC（系统解码器；WebP 需系统装有「Webp 图像扩展」）
//      - 两条路都失败 → 返回 **可操作** 的中文错误（decodeErrorMessage）
//  * 本模块不做任何 GL 调用；WIC 需要 COM，首次使用时按需 CoInitializeEx
//    （容忍 RPC_E_CHANGED_MODE：此时不配对 CoUninitialize）
//  * 诊断入口：`api_probe --image-decode <路径>`（只读，见 source/README.md §4.2）
// ============================================================================

#define AIWRITE_IMAGE_DECODE_FROZEN 1

#include <cstddef>
#include <string>
#include <vector>

namespace aiwrite::utils {

// 按内容识别的格式（Unknown = 魔数不认识，交给解码器自己嗅探）
enum class ImageFormat {
    Unknown,
    Png,
    Jpeg,
    Gif,
    Bmp,
    WebP,
    Tiff,
    Ico,
    JpegXr,
    Heif,
    Avif,
    Psd,
    Hdr,
    Pnm,
};

// "PNG" / "WebP" / ""（未知）
const char* imageFormatName(ImageFormat format);
// "image/png" / "image/webp" / ""（未知）
const char* imageFormatMime(ImageFormat format);

struct ImageMagic {
    ImageFormat format   = ImageFormat::Unknown; // 文件头判定的格式
    std::string ext_mime;                        // 扩展名推断的 MIME（回退用）
    std::string ext_label;                       // 小写扩展名（含点；空 = 无扩展名）
    bool        mismatch = false;                // 内容与扩展名都识别出来了但两者不一致
    std::string head_hex;                        // 文件头前 8 字节（十六进制；诊断用）
};

// 读文件头（32 字节）判定格式；文件打不开时 format = Unknown（ext_* 仍按扩展名填）
ImageMagic sniffImage(const std::string& path);
// 纯函数版本（header 不足 32 字节时按实际长度判定）
ImageMagic sniffImageBytes(const unsigned char* data, std::size_t size);
// 扩展名 → MIME（未知回退 image/png —— 保持 M5-02 既有语义）
std::string mimeFromExtension(const std::string& path);

struct DecodedImage {
    std::vector<unsigned char> rgba;    // width * height * 4（成功时）
    int         width   = 0;
    int         height  = 0;
    std::string decoder;                // "stb" / "wic"（成功时）
    std::string error;                  // 失败原因（成功时为空；已含可操作建议）
};

// 解码为 RGBA8（失败时 error 已可直接显示给用户）
DecodedImage decodeImageRgba8(const std::string& path);
// 只读尺寸（WIC 走 GetSize / stb 走 stbi_info；失败时 *error 同样可操作）
bool readImageSize(const std::string& path, int* width, int* height, std::string* error);

// 本机 WIC 能力（用于"装 Webp 图像扩展"这类提示；结果缓存）
struct WicInfo {
    bool        available = false; // WIC 工厂是否可用
    bool        webp      = false; // 系统是否注册了 WebP 解码器
    bool        heif      = false;
    bool        avif      = false;
    std::string note;              // 一句话摘要（Console / 面板可用）
};
WicInfo wicInfo();

// 失败文案（纯函数：便于离线断言）——组合 格式嗅探 / 扩展名 / 解码原因 生成建议
std::string decodeErrorMessage(const std::string& path, const ImageMagic& magic,
                               const std::string& reason);

} // namespace aiwrite::utils
