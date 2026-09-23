#pragma once

// ============================================================================
//  DeepSeek 网页版 PoW（设计 §8.5 / M4-09）
//
//  实测协议（2026-09-23，通过 web::request_protocol_probe 在已登录页面内取得）：
//    POST https://chat.deepseek.com/api/v0/chat/create_pow_challenge
//         headers: Authorization: Bearer <localStorage.userToken>
//         body:    {"target_path":"/api/v0/chat/completion"}
//      → {"code":0,"data":{"biz_data":{"challenge":{
//            "algorithm":"DeepSeekHashV1","challenge":"<64hex>","salt":"<hex>",
//            "signature":"<64hex>","difficulty":144000,"expire_at":<ms>,
//            "expire_after":300000,"target_path":"/api/v0/chat/completion"}}}}
//
//  求解：找最小 nonce，使 SHA3-256("{salt}_{expire_at}_{challenge}_{nonce}") 的
//        前 8 个十六进制位（32 bit）< difficulty；把结果放进请求头
//        `x-ds-pow-response: base64({"algorithm","challenge","salt","answer","signature","target_path"})`
// ============================================================================

#include <string>

namespace aiwrite::ai {

struct PowChallenge {
    std::string algorithm;   // "DeepSeekHashV1"
    std::string challenge;   // 64 hex
    std::string salt;        // hex
    std::string signature;   // 64 hex
    long long   difficulty = 0;
    long long   expire_at  = 0;   // 毫秒时间戳
    std::string target_path;
};

// 解析挑战响应；失败写 error
bool parse_pow_challenge(const std::string& json_text, PowChallenge* out, std::string* error);

// 求解；成功返回 nonce（十进制字符串）并写 attempts（尝试次数），失败返回空串
std::string solve_pow(const PowChallenge& challenge, long long* attempts, std::string* error);

// x-ds-pow-response 头值（base64(JSON)）
std::string make_pow_header_value(const PowChallenge& challenge, const std::string& answer);

// 标准 Base64（带 '=' 补齐）
std::string base64_encode(const std::string& data);

} // namespace aiwrite::ai
