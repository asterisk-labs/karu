#ifndef KARU_CREDENTIAL_CACHE_HPP
#define KARU_CREDENTIAL_CACHE_HPP

#include "config.hpp"
#include "request.hpp"

#include <cstdint>
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

    void invalidate(const ConfigSnapshot& config, const backends::CloudProvider& provider,
                    std::string_view path, bool custom);

  private:
    struct Entry {
        ProviderCredentials credentials;
        std::int64_t refresh_at = 0;
    };

    [[nodiscard]] static std::int64_t refresh_time(const ProviderCredentials& credentials,
                                                   std::int64_t now, bool refresh_static) noexcept;
    [[nodiscard]] static bool reusable(const Entry& entry, std::int64_t now) noexcept;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace karu

#endif
