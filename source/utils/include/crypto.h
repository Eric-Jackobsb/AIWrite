#pragma once

#include <string>
#include <string_view>

namespace aiwrite::crypto {

// SHA3-256，返回小写十六进制字符串（OpenSSL EVP）
std::string sha3_256(std::string_view data);

// SHA3-256 结果是否等于期望的十六进制串（大小写不敏感）
bool sha3_256_matches(std::string_view data, std::string_view expected_hex);

} // namespace aiwrite::crypto
