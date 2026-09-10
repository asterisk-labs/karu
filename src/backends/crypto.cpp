#include "crypto.hpp"

#include <limits>
#include <openssl/hmac.h>

namespace karu::backends {

std::expected<std::vector<unsigned char>, RequestError>
hmac(const EVP_MD* algorithm, std::span<const unsigned char> key, std::string_view value) {
    if (algorithm == nullptr)
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, "HMAC algorithm is unavailable"});
    const int digest_size = EVP_MD_get_size(algorithm);
    if (digest_size <= 0 ||
        key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "HMAC parameters are not supported"});
    }
    std::vector<unsigned char> result(static_cast<std::size_t>(digest_size));
    unsigned int length = 0;
    if (HMAC(algorithm, key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(value.data()), value.size(), result.data(),
             &length) == nullptr) {
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, "OpenSSL HMAC failed"});
    }
    result.resize(length);
    return result;
}

std::expected<std::vector<unsigned char>, RequestError>
hmac_sha256(std::span<const unsigned char> key, std::string_view value) {
    return hmac(EVP_sha256(), key, value);
}

std::string base64(std::span<const unsigned char> bytes) {
    if (bytes.empty())
        return {};
    std::string result(4 * ((bytes.size() + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                        bytes.data(), static_cast<int>(bytes.size()));
    result.resize(static_cast<std::size_t>(written));
    return result;
}

} // namespace karu::backends
