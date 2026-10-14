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

// 凭据是否**可用**（文件存在且能解密出非空 Key）
//  * 判据与 load_credential() 完全一致（含 magic/CRC/DPAPI 失败）——只是把**布尔结果**缓存起来
//  * 为什么需要：界面每帧都在问「这个节点有没有 Key」（节点卡片 / 参数面板 / 工具栏的
//    「必定失败」提示），而 load_credential() 每次都要读文件 + DPAPI 解密。
//    实测 85 节点 / 11 个 official 节点的基准图 ⇒ 每帧 11 次；缓存后为 0（TTL 到期才重新验一次）。
//  * 缓存**只存"是否可用"**，不驻留明文；保存/删除/清理时立即失效，另有 30 秒 TTL 兜底
//  * 需要真 Key 明文时仍走 load_credential()（不做缓存）
bool credential_available(const std::string& ref);

// 立即失效可用性缓存（ref 为空 = 全部清空）
//  * save/erase/purge 内部已自动调用；外部改动（手工删文件）由 30 秒 TTL 兜底
void invalidate_credential_cache(const std::string& ref);

// 凭据目录（M_patchAB §2.3 · `PB-06` 剩余：取自 `config.toml [credentials].dir`；
//  * 空 ⇒ 默认 `~/.brain-ai/credentials`）
std::string credentials_dir();

// 凭据**后端**是否可用（`[credentials].backend`）：空串 = 可用；非空 = 可操作错误文案
//  * 本版本只实现 `dpapi`（Windows DPAPI，当前用户作用域）⇒ 其它值**明确报错**（不静默降级）
std::string credentials_backend_error();

// Key 优先级：节点参数 → 环境变量 DEEPSEEK_API_KEY → 凭据库(ref)
ResolvedSecret resolve_secret(const std::string& param_key, const std::string& ref);

// 附录 B B.8 离线自检：返回失败条数（第 7 条 TTL 清理当前为“待修·不致命”，只出诊断）
int credential_selftest();

} // namespace aiwrite::utils
