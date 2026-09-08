#include "crypto.hpp"

#include <openssl/hmac.h>

namespace karu::backends {

std::vector<unsigned char> hmac(const EVP_MD* algorithm, std::span<const unsigned char> key,
                                std::string_view value) {
    std::vector<unsigned char> result(static_cast<std::size_t>(EVP_MD_get_size(algorithm)));
    unsigned int length = 0;
    HMAC(algorithm, key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(value.data()), value.size(), result.data(),
         &length);
    result.resize(length);
    return result;
}

std::vector<unsigned char> hmac_sha256(std::span<const unsigned char> key, std::string_view value) {
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
