#ifndef KARU_BACKENDS_CRYPTO_HPP
#define KARU_BACKENDS_CRYPTO_HPP

#include <openssl/evp.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace karu::backends {

[[nodiscard]] std::vector<unsigned char>
hmac(const EVP_MD* algorithm, std::span<const unsigned char> key, std::string_view value);
[[nodiscard]] std::vector<unsigned char> hmac_sha256(std::span<const unsigned char> key,
                                                     std::string_view value);
[[nodiscard]] std::string base64(std::span<const unsigned char> bytes);

} // namespace karu::backends

#endif
