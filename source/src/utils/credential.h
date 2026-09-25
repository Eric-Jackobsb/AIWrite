#pragma once

// ============================================================================
//  凭据库（PB-06 / 设计见 docs/ai_writer_nodes.md 附录 B）
//
//  * 存储：~/.brain-ai/credentials/<safe-ref>.bin（专用目录 + ACL 收紧）
//  * 加密：Windows DPAPI CryptProtectData（当前用户作用域，不开 LOCAL_MACHINE）
//  * 容器：BRNC v1（magic/version/ref_hash/时间戳/cipher_len/cipher/crc32）
//  * 保密：无明文落盘、无日志回显、解密失败视为无 Key（不降级明文）
// ============================================================================

#include <string>
#include <vector>

namespace aiwrite::utils {

struct CredentialInfo {
    std::string ref;
    long long   updated_at         = 0;
    int         ttl_remaining_days = 0;
};

// 三级优先级解析结果（source: node | env | store | none）
struct ResolvedSecret {
    std::string key;    // 明文（仅内存；调用方用完即弃）
    std::string source;
};

bool                save_credential(const std::string& ref, const std::string& secret, std::string* error);
std::string         load_credential(const std::string& ref, std::string* error); // 失败返回空串
bool                erase_credential(const std::string& ref, std::string* error);
std::vector<CredentialInfo> list_credentials(int ttl_days);
int                 purge_expired_credentials(int ttl_days, std::string* error);

// 凭据目录（默认 ~/.brain-ai/credentials）
std::string credentials_dir();

// Key 优先级：节点参数 → 环境变量 DEEPSEEK_API_KEY → 凭据库(ref)
ResolvedSecret resolve_secret(const std::string& param_key, const std::string& ref);

} // namespace aiwrite::utils
