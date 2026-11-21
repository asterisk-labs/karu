#include "request_builder.hpp"

#include "backends/contract.hpp"
#include "backends/credentials.hpp"
#include "backends/s3_signing.hpp"

#include <ctime>

namespace karu {
namespace {

constexpr std::size_t kMaxRegions = 1024;

// A bucket name can exist behind two endpoints, each with its own region.
std::string region_key(const ConfigSnapshot& config, const Resolved& object) {
    std::string key = config.option(object.canonical_uri, "AWS_S3_ENDPOINT");
    key.push_back('\n');
    key += object.container;
    return key;
}

void apply_http_options(const ConfigSnapshot& config, PreparedRequest& request) {
    const bool follow_redirects = request.http.follow_redirects;
    request.http = config.http_options();
    request.http.follow_redirects = follow_redirects;
    request.http.same_origin_redirects_only = config.has_configured_headers();
}

} // namespace

std::expected<PreparedRequest, RequestError>
RequestBuilder::prepare(const Locator& locator, std::uint64_t first, std::uint64_t length,
                        std::string_view region_hint, std::string_view if_match) {
    if (length == 0 || first > UINT64_MAX - (length - 1))
        return std::unexpected(RequestError{KARU_ERR_RANGE, "HTTP range overflows"});
    auto credentials = resolve_credentials(locator);
    if (!credentials)
        return std::unexpected(credentials.error());
    return materialize(locator, *credentials, first, length, region_hint, if_match);
}

std::expected<ResolvedCredentials, RequestError>
RequestBuilder::resolve_credentials(const Locator& locator) {
    const Resolved& object = locator.resolved;
    if (object.backend == Backend::Http || object.backend == Backend::HuggingFace)
        return ResolvedCredentials{};

    const backends::CloudProvider* provider = backends::cloud_provider(object.backend);
    if (provider == nullptr)
        return std::unexpected(RequestError{KARU_ERR_UNSUPPORTED, "unsupported request backend"});

    const auto custom_provider = config_.provider(provider->credentials_kind);
    bool credentials_configured = custom_provider != nullptr;
    if (provider->anonymous_by_default && !credentials_configured) {
        for (std::string_view option : provider->credential_options) {
            if (config_.has_option(object.canonical_uri, option)) {
                credentials_configured = true;
                break;
            }
        }
    }

    const bool no_sign =
        config_.has_option(object.canonical_uri, provider->no_sign_option)
            ? option_is_true(config_.option(object.canonical_uri, provider->no_sign_option))
            : provider->anonymous_by_default && !credentials_configured;
    if (no_sign)
        return ResolvedCredentials{};

    auto loaded = custom_provider ? credentials_.custom(config_, provider->credentials_kind,
                                                        object.canonical_uri)
                                  : credentials_.native(config_, *provider, object.canonical_uri);
    if (!loaded)
        return std::unexpected(loaded.error());
    return ResolvedCredentials{std::move(*loaded)};
}

std::expected<PreparedRequest, RequestError>
RequestBuilder::materialize(const Locator& locator, const ResolvedCredentials& credentials,
                            std::uint64_t first, std::uint64_t length, std::string_view region_hint,
                            std::string_view if_match) {
    if (length == 0 || first > UINT64_MAX - (length - 1))
        return std::unexpected(RequestError{KARU_ERR_RANGE, "HTTP range overflows"});
    return materialize_range(locator, credentials, backends::range_header(first, length),
                             region_hint, if_match);
}

std::expected<PreparedRequest, RequestError>
RequestBuilder::prepare_suffix(const Locator& locator, std::uint64_t length,
                               std::string_view region_hint, std::string_view if_match) {
    if (length == 0)
        return std::unexpected(RequestError{KARU_ERR_RANGE, "empty suffix range"});
    auto credentials = resolve_credentials(locator);
    if (!credentials)
        return std::unexpected(credentials.error());
    return materialize_range(locator, *credentials, backends::suffix_range_header(length),
                             region_hint, if_match);
}

std::expected<PreparedRequest, RequestError>
RequestBuilder::materialize_range(const Locator& locator, const ResolvedCredentials& credentials,
                                  std::string range, std::string_view region_hint,
                                  std::string_view if_match) {
    const Resolved& object = locator.resolved;
    if (object.backend == Backend::Http || object.backend == Backend::HuggingFace) {
        auto prepared = object.backend == Backend::Http
                            ? backends::prepare_http(config_, object, if_match)
                            : backends::prepare_hugging_face(config_, object, if_match);
        if (prepared) {
            prepared->range = std::move(range);
            apply_http_options(config_, *prepared);
        }
        return prepared;
    }

    const backends::CloudProvider* provider = backends::cloud_provider(object.backend);
    if (provider == nullptr)
        return std::unexpected(RequestError{KARU_ERR_UNSUPPORTED, "unsupported request backend"});

    // The signature must see the final range and routing region. Retries call
    // materialize again after either one changes.
    backends::RequestContext request{config_,
                                     object,
                                     credentials.value ? &*credentials.value : nullptr,
                                     range,
                                     region_hint,
                                     if_match,
                                     std::time(nullptr)};
    auto prepared = provider->prepare_request(request);
    if (!prepared)
        return std::unexpected(prepared.error());
    if (auto headers = backends::add_configured_headers(config_, object.canonical_uri,
                                                        prepared->headers, true);
        !headers) {
        return std::unexpected(headers.error());
    }
    prepared->range = std::move(range);
    apply_http_options(config_, *prepared);
    return prepared;
}

std::string RequestBuilder::known_region(const Locator& locator) {
    if (locator.resolved.backend != Backend::S3)
        return {};
    const std::string key = region_key(config_, locator.resolved);
    std::lock_guard lock(regions_mutex_);
    const auto found = regions_.find(key);
    return found == regions_.end() ? std::string{} : found->second;
}

void RequestBuilder::remember_region(const Locator& locator, std::string_view region) {
    // The region comes from a response, so it is checked like a configured one.
    if (locator.resolved.backend != Backend::S3 || !backends::valid_aws_region(region))
        return;
    std::string key = region_key(config_, locator.resolved);
    std::lock_guard lock(regions_mutex_);
    if (regions_.size() >= kMaxRegions && !regions_.contains(key))
        regions_.clear();
    regions_.insert_or_assign(std::move(key), std::string(region));
}

void RequestBuilder::invalidate_credentials(const Locator& locator) {
    const backends::CloudProvider* provider = backends::cloud_provider(locator.resolved.backend);
    if (provider == nullptr)
        return;
    const bool custom = config_.provider(provider->credentials_kind) != nullptr;
    credentials_.invalidate(config_, *provider, locator.resolved.canonical_uri, custom);
}

} // namespace karu
