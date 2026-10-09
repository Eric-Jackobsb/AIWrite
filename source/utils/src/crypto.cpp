#include "utils/crypto.h"

#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <memory>

namespace aiwrite::crypto {

std::string sha3_256(std::string_view data)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx) {
        return {};
    }

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha3_256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1) {
        return {};
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<std::size_t>(digest_len) * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        result.push_back(kHex[digest[i] >> 4]);
        result.push_back(kHex[digest[i] & 0x0F]);
    }
    return result;
}

bool sha3_256_matches(std::string_view data, std::string_view expected_hex)
{
    const std::string actual = sha3_256(data);
    if (actual.size() != expected_hex.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto a = static_cast<unsigned char>(actual[i]);
        const auto b = static_cast<unsigned char>(expected_hex[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

} // namespace aiwrite::crypto
