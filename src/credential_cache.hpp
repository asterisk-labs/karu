#ifndef KARU_CREDENTIAL_CACHE_HPP
#define KARU_CREDENTIAL_CACHE_HPP

#include "config.hpp"
#include "request.hpp"

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
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

    struct Flight {
        std::condition_variable ready;
        bool done = false;
        bool invalidated = false;
        std::optional<std::expected<ProviderCredentials, RequestError>> result;
    };

    [[nodiscard]] static std::int64_t refresh_time(const ProviderCredentials& credentials,
                                                   std::int64_t now, bool refresh_static) noexcept;
    [[nodiscard]] static bool reusable(const Entry& entry, std::int64_t now) noexcept;

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, std::shared_ptr<Flight>> flights_;
};

} // namespace karu

#endif
