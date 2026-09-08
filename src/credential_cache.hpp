#ifndef KARU_CREDENTIAL_CACHE_HPP
#define KARU_CREDENTIAL_CACHE_HPP

#include "config.hpp"
#include "request.hpp"

#include <expected>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace karu {

namespace backends {
struct CloudProvider;
}

class CredentialCache {
  public:
    [[nodiscard]] std::expected<ProviderCredentials, RequestError>
    custom(const ConfigSnapshot& config, karu_credentials_kind kind, std::string_view path);

    [[nodiscard]] std::expected<ProviderCredentials, RequestError>
    native(const ConfigSnapshot& config, const backends::CloudProvider& provider,
           std::string_view path);

  private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ProviderCredentials> entries_;
};

} // namespace karu

#endif
