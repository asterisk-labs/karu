#include "credential_cache.hpp"

#include "backends/contract.hpp"
#include "backends/credentials.hpp"
#include "uri.hpp"

#include <algorithm>
#include <ctime>
#include <limits>
#include <mutex>

namespace karu {
namespace {

constexpr std::int64_t kStaticCredentialTtlSeconds = 60;

} // namespace

std::int64_t CredentialCache::refresh_time(const ProviderCredentials& credentials, std::int64_t now,
                                           bool refresh_static) noexcept {
    if (credentials.expires_at == 0) {
        return refresh_static ? now + kStaticCredentialTtlSeconds
                              : std::numeric_limits<std::int64_t>::max();
    }
    if (credentials.expires_at <= now)
        return now;
    const std::int64_t lifetime = credentials.expires_at - now;
    const std::int64_t margin = std::clamp<std::int64_t>(lifetime / 10, 1, 60);
    return std::max<std::int64_t>(now + 1, credentials.expires_at - margin);
}

bool CredentialCache::reusable(const Entry& entry, std::int64_t now) noexcept {
    return now < entry.refresh_at &&
           (entry.credentials.expires_at == 0 || now < entry.credentials.expires_at);
}

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
            const auto& [key, entry] = *iterator;
            const auto& value = entry.credentials;
            const bool current = key.starts_with(family) && path.starts_with(value.cache_prefix) &&
                                 reusable(entry, now);
            if (current &&
                (best == entries_.end() ||
                 value.cache_prefix.size() > best->second.credentials.cache_prefix.size())) {
                best = iterator;
            }
        }
        return best;
    };

    {
        std::shared_lock lock(mutex_);
        const auto found = find_reusable();
        if (found != entries_.end())
            return found->second.credentials;
    }
    std::unique_lock lock(mutex_);
    const auto found = find_reusable();
    if (found != entries_.end())
        return found->second.credentials;

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
        entries_[family + refreshed->cache_prefix] =
            Entry{*refreshed, refresh_time(*refreshed, now, false)};
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
        if (found != entries_.end() && reusable(found->second, now)) {
            return found->second.credentials;
        }
    }

    std::unique_lock lock(mutex_);
    const auto found = entries_.find(key);
    if (found != entries_.end() && reusable(found->second, now)) {
        return found->second.credentials;
    }

    auto loaded = provider.load_credentials(config, path);
    if (!loaded)
        return std::unexpected(loaded.error());
    const bool present = !loaded->access_key_id.empty() || !loaded->secret_access_key.empty() ||
                         !loaded->session_token.empty() || !loaded->bearer_token.empty() ||
                         !loaded->sas_token.empty();
    // Never negative-cache credential discovery. One anonymous/missing lookup
    // must not poison later signed requests (the failure reported in GDAL #11964).
    if (present && (loaded->expires_at == 0 || loaded->expires_at > now))
        entries_[key] = Entry{*loaded, refresh_time(*loaded, now, true)};
    return *loaded;
}

void CredentialCache::invalidate(const ConfigSnapshot& config,
                                 const backends::CloudProvider& provider, std::string_view path,
                                 bool custom) {
    std::unique_lock lock(mutex_);
    if (!custom) {
        const std::string key = "native\n" +
                                std::to_string(static_cast<int>(provider.credentials_kind)) + "\n" +
                                config.scope_key(path, provider.credential_options);
        entries_.erase(key);
        return;
    }

    const std::string family =
        "custom\n" + std::to_string(static_cast<int>(provider.credentials_kind)) + "\n";
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        if (iterator->first.starts_with(family) &&
            path.starts_with(iterator->second.credentials.cache_prefix)) {
            iterator = entries_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

} // namespace karu
