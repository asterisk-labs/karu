#include "credential_cache.hpp"

#include "backends/contract.hpp"
#include "backends/credentials.hpp"
#include "uri.hpp"

#include <ctime>
#include <mutex>

namespace karu {

std::expected<ProviderCredentials, RequestError>
CredentialCache::custom(const ConfigSnapshot& config, karu_credentials_kind kind,
                        std::string_view path) {
    const auto callback = config.provider(kind);
    if (!callback)
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, {}});

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::string family = "custom\n" + std::to_string(static_cast<int>(kind)) + "\n";
    const auto find_reusable = [&] {
        auto best = entries_.end();
        for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
            const auto& [key, value] = *iterator;
            const bool current = key.starts_with(family) && path.starts_with(value.cache_prefix) &&
                                 (value.expires_at == 0 || value.expires_at > now + 60);
            if (current && (best == entries_.end() ||
                            value.cache_prefix.size() > best->second.cache_prefix.size())) {
                best = iterator;
            }
        }
        return best;
    };

    {
        std::shared_lock lock(mutex_);
        const auto found = find_reusable();
        if (found != entries_.end())
            return found->second;
    }
    std::unique_lock lock(mutex_);
    const auto found = find_reusable();
    if (found != entries_.end())
        return found->second;

    auto refreshed = backends::copy_callback_credentials(callback, kind, path);
    if (!refreshed)
        return std::unexpected(refreshed.error());
    if (!refreshed->cache_prefix.empty()) {
        refreshed->cache_prefix = canonical_vsi_path(refreshed->cache_prefix);
        if (!path.starts_with(refreshed->cache_prefix)) {
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS,
                             "custom credential cache_prefix does not contain the requested path"});
        }
        entries_[family + refreshed->cache_prefix] = *refreshed;
    }
    return *refreshed;
}

std::expected<ProviderCredentials, RequestError>
CredentialCache::native(const ConfigSnapshot& config, const backends::CloudProvider& provider,
                        std::string_view path) {
    const std::string key = "native\n" +
                            std::to_string(static_cast<int>(provider.credentials_kind)) + "\n" +
                            config.scope_key(path, provider.credential_options);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    {
        std::shared_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end() &&
            (found->second.expires_at == 0 || found->second.expires_at > now + 60)) {
            return found->second;
        }
    }

    std::unique_lock lock(mutex_);
    const auto found = entries_.find(key);
    if (found != entries_.end() &&
        (found->second.expires_at == 0 || found->second.expires_at > now + 60)) {
        return found->second;
    }

    auto loaded = provider.load_credentials(config, path);
    if (!loaded)
        return std::unexpected(loaded.error());
    const bool present = !loaded->access_key_id.empty() || !loaded->secret_access_key.empty() ||
                         !loaded->session_token.empty() || !loaded->bearer_token.empty() ||
                         !loaded->sas_token.empty();
    // Never negative-cache credential discovery. One anonymous/missing lookup
    // must not poison later signed requests (the failure reported in GDAL #11964).
    if (present && loaded->expires_at != 0)
        entries_[key] = *loaded;
    return *loaded;
}

} // namespace karu
