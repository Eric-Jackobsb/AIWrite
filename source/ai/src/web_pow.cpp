#include "ai/web_pow.h"

#include "utils/crypto.h"

#include <exception>

#include <nlohmann/json.hpp>

namespace aiwrite::ai {
namespace {

// 十六进制统一为小写（服务端 challenge 为小写；OpenSSL 输出亦为小写）
std::string to_lower_hex(const std::string& text)
{
    std::string out = text;
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'F') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

} // namespace

std::string base64_encode(const std::string& data)
{
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    std::size_t index = 0;
    while (index + 3 <= data.size()) {
        const auto b0 = static_cast<unsigned char>(data[index]);
        const auto b1 = static_cast<unsigned char>(data[index + 1]);
        const auto b2 = static_cast<unsigned char>(data[index + 2]);
        out += kTable[b0 >> 2];
        out += kTable[static_cast<std::size_t>(((b0 & 0x03U) << 4) | (b1 >> 4))];
        out += kTable[static_cast<std::size_t>(((b1 & 0x0FU) << 2) | (b2 >> 6))];
        out += kTable[b2 & 0x3FU];
        index += 3;
    }

    const std::size_t remain = data.size() - index;
    if (remain == 1) {
        const auto b0 = static_cast<unsigned char>(data[index]);
        out += kTable[b0 >> 2];
        out += kTable[static_cast<std::size_t>((b0 & 0x03U) << 4)];
        out += "==";
    }
    else if (remain == 2) {
        const auto b0 = static_cast<unsigned char>(data[index]);
        const auto b1 = static_cast<unsigned char>(data[index + 1]);
        out += kTable[b0 >> 2];
        out += kTable[static_cast<std::size_t>(((b0 & 0x03U) << 4) | (b1 >> 4))];
        out += kTable[static_cast<std::size_t>((b1 & 0x0FU) << 2)];
        out += '=';
    }
    return out;
}

bool parse_pow_challenge(const std::string& json_text, PowChallenge* out, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    if (out == nullptr) {
        return fail("输出参数为空");
    }

    try {
        const nlohmann::json json = nlohmann::json::parse(json_text);
        if (json.value("code", -1) != 0) {
            return fail("挑战接口返回失败：" + json.value("msg", std::string("code != 0")));
        }
        if (!json.contains("data")) {
            return fail("挑战响应缺少 data");
        }
        const nlohmann::json& data = json["data"];
        if (!data.contains("biz_data") || !data["biz_data"].contains("challenge")) {
            return fail("挑战响应缺少 data.biz_data.challenge");
        }

        const nlohmann::json& node = data["biz_data"]["challenge"];
        out->algorithm   = node.value("algorithm", std::string());
        out->challenge   = node.value("challenge", std::string());
        out->salt        = node.value("salt", std::string());
        out->signature   = node.value("signature", std::string());
        out->difficulty  = node.value("difficulty", 0LL);
        out->expire_at   = node.value("expire_at", 0LL);
        out->target_path = node.value("target_path", std::string());

        if (out->challenge.empty() || out->salt.empty() || out->difficulty <= 0) {
            return fail("挑战字段不完整（challenge / salt / difficulty）");
        }
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    catch (const std::exception& ex) {
        return fail(std::string("挑战 JSON 解析失败: ") + ex.what());
    }
}

std::string solve_pow(const PowChallenge& challenge, long long* attempts, std::string* error)
{
    if (attempts != nullptr) {
        *attempts = 0;
    }

    // 官方实现（静态资源 static/76608.*.js 的 PoW worker）：
    //   prefix = "{salt}_{expire_at}_"
    //   for (nonce = 0; nonce < difficulty; ++nonce)
    //       if (SHA3-256(prefix + nonce) == challenge) return nonce;   // challenge 即目标哈希
    const std::string prefix =
        challenge.salt + "_" + std::to_string(challenge.expire_at) + "_";
    const std::string target = to_lower_hex(challenge.challenge);

    const long long limit = challenge.difficulty;
    for (long long nonce = 0; nonce < limit; ++nonce) {
        if (attempts != nullptr) {
            *attempts = nonce + 1;
        }
        if (crypto::sha3_256(prefix + std::to_string(nonce)) == target) {
            if (error != nullptr) {
                error->clear();
            }
            return std::to_string(nonce);
        }
    }

    if (error != nullptr) {
        *error = "在 difficulty(" + std::to_string(limit) + ") 范围内未找到匹配 nonce（可重新取挑战）";
    }
    return {};
}

std::string make_pow_header_value(const PowChallenge& challenge, const std::string& answer)
{
    nlohmann::json json;
    json["algorithm"]   = challenge.algorithm.empty() ? std::string("DeepSeekHashV1")
                                                      : challenge.algorithm;
    json["challenge"]   = challenge.challenge;
    json["salt"]        = challenge.salt;
    json["answer"]      = answer.empty() ? 0 : std::stoll(answer);
    json["signature"]   = challenge.signature;
    json["target_path"] = challenge.target_path;
    return base64_encode(json.dump());
}

} // namespace aiwrite::ai
