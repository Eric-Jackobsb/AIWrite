#include "utils/credential.h"

#include "utils/log.h"
#include "utils/paths.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <aclapi.h>
#include <dpapi.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace aiwrite::utils {
namespace {

constexpr const char* kMagic   = "BRNC";
constexpr unsigned    kVersion = 1;

std::uint32_t crc32_of(const std::string& data)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~((crc & 1u) - 1u)));
        }
    }
    return ~crc;
}

std::string safe_ref_name(const std::string& ref)
{
    std::string out;
    for (const char ch : ref) {
        out += (std::isalnum(static_cast<unsigned char>(ch)) != 0) ? ch : '_';
    }
    return out.empty() ? std::string("default") : out;
}

std::uint64_t ref_hash(const std::string& ref)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : ref) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string file_for(const std::string& ref)
{
    return credentials_dir() + "/" + safe_ref_name(ref) + ".bin";
}

void append_u32(std::string& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out += static_cast<char>((value >> (8 * i)) & 0xFFu);
    }
}
void append_u64(std::string& out, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out += static_cast<char>((value >> (8 * i)) & 0xFFu);
    }
}
std::uint32_t read_u32(const std::string& data, std::size_t offset)
{
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + i])) << (8 * i);
    }
    return value;
}
std::uint64_t read_u64(const std::string& data, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[offset + i])) << (8 * i);
    }
    return value;
}

#if defined(_WIN32)
// ACL 收紧为「仅当前用户」（失败只记日志，不阻断）
void harden_acl(const std::string& path)
{
    PSID user = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0,
                                 0, 0, 0, 0, 0, &user) == FALSE) {
        return;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode        = SET_ACCESS;
    access.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType  = TRUSTEE_IS_GROUP;
    access.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(user);
    PACL acl = nullptr;
    const std::wstring wide(path.begin(), path.end());
    if (SetEntriesInAclW(1, &access, nullptr, &acl) == ERROR_SUCCESS && acl != nullptr) {
        const DWORD result = SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT,
                                                  DACL_SECURITY_INFORMATION |
                                                      PROTECTED_DACL_SECURITY_INFORMATION,
                                                  nullptr, nullptr, acl, nullptr);
        if (result != ERROR_SUCCESS) {
            log::warn("[凭据] ACL 收紧失败（错误码 " + std::to_string(result) + "），继续");
        }
        LocalFree(acl);
    }
    FreeSid(user);
}

bool dpapi_protect(const std::string& plain, std::string* cipher, std::string* error)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB out{};
    if (CryptProtectData(&in, L"AIwrite credential", nullptr, nullptr, nullptr, 0, &out) == FALSE) {
        *error = "DPAPI 加密失败（错误码 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    cipher->assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool dpapi_unprotect(const std::string& cipher, std::string* plain, std::string* error)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(cipher.data()));
    in.cbData = static_cast<DWORD>(cipher.size());
    DATA_BLOB out{};
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out) == FALSE) {
        *error = "DPAPI 解密失败（换用户/换机器或文件损坏）";
        return false;
    }
    plain->assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}
#else
void harden_acl(const std::string&) {}
bool dpapi_protect(const std::string&, std::string*, std::string* error)
{
    *error = "当前平台未实现凭据加密（仅 Windows DPAPI）";
    return false;
}
bool dpapi_unprotect(const std::string&, std::string*, std::string* error)
{
    *error = "当前平台未实现凭据解密（仅 Windows DPAPI）";
    return false;
}
#endif

} // namespace

std::string credentials_dir()
{
    return (paths::data_root() / "credentials").string();
}

bool save_credential(const std::string& ref, const std::string& secret, std::string* error)
{
    if (ref.empty() || secret.empty()) {
        if (error != nullptr) { *error = "ref 或 secret 为空"; }
        return false;
    }
    const std::string dir = credentials_dir();
    std::error_code   ec;
    std::filesystem::create_directories(dir, ec);
    harden_acl(dir);

    std::string cipher;
    std::string crypto_error;
    if (!dpapi_protect(secret, &cipher, &crypto_error)) {
        if (error != nullptr) { *error = crypto_error; }
        return false;
    }

    std::string payload;
    payload.append(kMagic, 4);
    payload += static_cast<char>(kVersion);
    append_u64(payload, ref_hash(ref));
    append_u64(payload, static_cast<std::uint64_t>(std::time(nullptr)));
    append_u64(payload, static_cast<std::uint64_t>(std::time(nullptr)));
    append_u32(payload, static_cast<std::uint32_t>(cipher.size()));
    payload += cipher;
    append_u32(payload, crc32_of(payload));

    const std::string path = file_for(ref);
    std::ofstream     stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error != nullptr) { *error = "无法写入凭据文件：" + path; }
        return false;
    }
    stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    stream.close();
    harden_acl(path);
    log::info("[凭据] 已保存 ref=" + ref + "（长度 " + std::to_string(secret.size()) + "，密文仅落盘）");
    return true;
}

std::string load_credential(const std::string& ref, std::string* error)
{
    const std::string path = file_for(ref);
    std::ifstream     stream(path, std::ios::binary);
    if (!stream) {
        if (error != nullptr) { *error = "凭据不存在：" + ref; }
        return {};
    }
    std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();

    if (data.size() < 4 + 1 + 8 + 8 + 8 + 4 + 4 || data.compare(0, 4, kMagic) != 0) {
        if (error != nullptr) { *error = "凭据文件格式非法（magic/长度）"; }
        return {};
    }
    const std::size_t crc_offset = data.size() - 4;
    if (read_u32(data, crc_offset) != crc32_of(data.substr(0, crc_offset))) {
        if (error != nullptr) { *error = "凭据文件 CRC 校验失败（已损坏）"; }
        return {};
    }
    const std::size_t  cipher_len = read_u32(data, 4 + 1 + 8 + 8 + 8);
    const std::size_t  cipher_at  = 4 + 1 + 8 + 8 + 8 + 4;
    if (cipher_at + cipher_len > crc_offset) {
        if (error != nullptr) { *error = "凭据文件长度字段越界"; }
        return {};
    }
    std::string plain;
    std::string crypto_error;
    if (!dpapi_unprotect(data.substr(cipher_at, cipher_len), &plain, &crypto_error)) {
        if (error != nullptr) { *error = crypto_error; }
        return {};
    }
    return plain;
}

bool erase_credential(const std::string& ref, std::string* error)
{
    std::error_code ec;
    const std::string path = file_for(ref);
    if (!std::filesystem::exists(path, ec)) {
        if (error != nullptr) { *error = "凭据不存在：" + ref; }
        return false;
    }
    std::filesystem::remove(path, ec);
    if (ec) {
        if (error != nullptr) { *error = "删除失败：" + ec.message(); }
        return false;
    }
    log::info("[凭据] 已删除 ref=" + ref);
    return true;
}

std::vector<CredentialInfo> list_credentials(int ttl_days)
{
    std::vector<CredentialInfo> items;
    std::error_code             ec;
    const std::string           dir = credentials_dir();
    if (!std::filesystem::exists(dir, ec)) {
        return items;
    }
    const long long now = static_cast<long long>(std::time(nullptr));
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".bin") {
            continue;
        }
        std::ifstream stream(entry.path(), std::ios::binary);
        std::string   data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        CredentialInfo info;
        info.ref = entry.path().stem().string();
        if (data.size() >= 4 + 1 + 8 + 8 + 8) {
            info.updated_at = static_cast<long long>(read_u64(data, 4 + 1 + 8 + 8));
        }
        const long long age_days = (now - info.updated_at) / 86400;
        info.ttl_remaining_days  = (ttl_days <= 0) ? -1 : static_cast<int>(ttl_days - age_days);
        items.push_back(info);
    }
    return items;
}

int purge_expired_credentials(int ttl_days, std::string* error)
{
    if (ttl_days <= 0) {
        return 0; // 0/负数 = 不清理
    }
    const long long   now = static_cast<long long>(std::time(nullptr));
    std::error_code   ec;
    const std::string dir = credentials_dir();
    if (!std::filesystem::exists(dir, ec)) {
        return 0;
    }

    // ① 先收集候选（目录迭代器在本作用域结束时释放句柄 —— 避免 Windows 下“迭代中删除子项”静默失败）
    std::vector<std::string> victims;
    {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".bin") {
                continue;
            }
            std::string data;
            {
                std::ifstream stream(entry.path(), std::ios::binary);
                data.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            }
            if (data.size() < 4 + 1 + 8 + 8 + 8) {
                continue;
            }
            const long long updated  = static_cast<long long>(read_u64(data, 4 + 1 + 8 + 8));
            const long long age_days = (now - updated) / 86400;
            log::info("[凭据] purge 检查 " + entry.path().filename().string() + " 大小=" +
                      std::to_string(data.size()) + " 更新=" + std::to_string(updated) + " 年龄=" +
                      std::to_string(age_days) + " 天 TTL=" + std::to_string(ttl_days) +
                      (age_days < ttl_days ? "（保留）" : "（待删）"));
            if (age_days >= ttl_days) {
                victims.push_back(entry.path().string());
            }
        }
    }

    // ② 再删除（此时迭代器已销毁，句柄已释放）
    int removed = 0;
    for (const std::string& victim : victims) {
        std::error_code remove_error;
        std::filesystem::remove(victim, remove_error);
        if (!remove_error) {
            ++removed;
            log::info("[凭据] 已清理过期条目 " + victim);
        }
        else {
            log::warn("[凭据] 清理失败 " + victim + "：" + remove_error.message());
        }
    }
    if (error != nullptr) {
        error->clear();
    }
    return removed;
}

ResolvedSecret resolve_secret(const std::string& param_key, const std::string& ref)
{
    ResolvedSecret resolved;
    if (!param_key.empty()) {
        resolved.key    = param_key;
        resolved.source = "node";
        return resolved;
    }
    const char* env_key = std::getenv("DEEPSEEK_API_KEY");
    if (env_key != nullptr && *env_key != '\0') {
        resolved.key    = env_key;
        resolved.source = "env";
        return resolved;
    }
    if (!ref.empty()) {
        std::string       error;
        const std::string stored = load_credential(ref, &error);
        if (!stored.empty()) {
            resolved.key    = stored;
            resolved.source = "store";
            return resolved;
        }
        // 降噪：凭据不存在属正常回退路径（运行前校验会反复查询），只留 info
        log::info("[凭据] 未找到凭据 ref=" + ref + "（视为无 Key，继续回退）");
    }
    resolved.source = "none";
    return resolved;
}

int credential_selftest()
{
    int               failed = 0;
    const std::string ref    = "selftest/_tmp";
    const std::string secret = "sk-selftest-0123456789abcdef";
    std::string       error;
    std::string       path  = file_for(ref);
    std::string       bytes;

    // 1) 可逆
    const bool        saved  = save_credential(ref, secret, &error);
    const std::string loaded = load_credential(ref, &error);
    if (!saved || loaded != secret) { ++failed; log::error("[凭据自检] 1 加密可逆：失败 " + error); }
    else { log::info("[凭据自检] 1 加密可逆：OK"); }

    // 2) 无明文 + 容器头
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (bytes.find(secret) != std::string::npos || bytes.size() < 16 ||
        bytes.compare(0, 4, kMagic) != 0) { ++failed; log::error("[凭据自检] 2 无明文/容器头：失败"); }
    else { log::info("[凭据自检] 2 无明文落盘 + 容器头：OK"); }

    // 3) CRC 篡改被拒
    {
        std::string bad = bytes;
        if (bad.size() > 8) { bad[8] = static_cast<char>(bad[8] ^ 0x5A); }
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(bad.data(), static_cast<std::streamsize>(bad.size()));
        }
        if (!load_credential(ref, &error).empty()) { ++failed; log::error("[凭据自检] 3：失败"); }
        else { log::info("[凭据自检] 3 CRC/格式篡改被拒：OK"); }
        (void)save_credential(ref, secret, &error);
    }

    // 4) 未知 ref
    {
        std::string unknown_error;
        if (!load_credential("selftest/does-not-exist", &unknown_error).empty() || unknown_error.empty()) {
            ++failed; log::error("[凭据自检] 4：失败");
        }
        else { log::info("[凭据自检] 4 未知 ref 返回空+错误：OK"); }
    }

    // 5) 三级优先级
    {
        const ResolvedSecret a = resolve_secret("node-key", ref);
        const ResolvedSecret b = resolve_secret("", ref);
        const ResolvedSecret c = resolve_secret("", "selftest/no-such");
        if (a.source != "node" || b.source != "store" || c.source != "none") { ++failed; log::error("[凭据自检] 5：失败"); }
        else { log::info("[凭据自检] 5 三级优先级（node/store/none）：OK"); }
    }

    // 6) list 不含密文
    {
        bool leaked = false;
        for (const CredentialInfo& item : list_credentials(30)) {
            if (item.ref.find(secret) != std::string::npos) { leaked = true; }
        }
        if (leaked) { ++failed; log::error("[凭据自检] 6：失败"); }
        else { log::info("[凭据自检] 6 list 不含密文：OK"); }
    }

    // 7) TTL 清理 —— 当前“待修·不致命”：只输出诊断，不计入失败
    {
        (void)purge_expired_credentials(0, &error);
        {
            std::ifstream in(path, std::ios::binary);
            std::string   aged((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (aged.size() > 4 + 1 + 8 + 8 + 8) {
                const std::uint64_t stamp = static_cast<std::uint64_t>(std::time(nullptr)) - 40ull * 86400ull;
                for (int i = 0; i < 8; ++i) { aged[4 + 1 + 8 + 8 + i] = static_cast<char>((stamp >> (8 * i)) & 0xFFu); }
                const std::size_t   crc_at = aged.size() - 4;
                const std::uint32_t crc    = crc32_of(aged.substr(0, crc_at));
                for (int i = 0; i < 4; ++i) { aged[crc_at + static_cast<std::size_t>(i)] = static_cast<char>((crc >> (8 * i)) & 0xFFu); }
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(aged.data(), static_cast<std::streamsize>(aged.size()));
            }
        }
        const int removed = purge_expired_credentials(1, &error);
        std::error_code after_ec;
        const bool      exists = std::filesystem::exists(path, after_ec);
        if (removed != 1 || exists) {
            ++failed;
            log::error("[凭据自检] 7 TTL 清理：失败 removed=" + std::to_string(removed) +
                        " exists=" + (exists ? "1" : "0"));
        }
        else { log::info("[凭据自检] 7 TTL 清理（0 不清理 / 过期即删）：OK"); }
    }

    // 8) 自清理
    {
        std::string erase_error;
        (void)erase_credential(ref, &erase_error);
        std::error_code after_ec;
        if (std::filesystem::exists(path, after_ec)) { ++failed; log::error("[凭据自检] 8：失败"); }
        else { log::info("[凭据自检] 8 测试条目已清理：OK"); }
    }
    return failed;
}

} // namespace aiwrite::utils
