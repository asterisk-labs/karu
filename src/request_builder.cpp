#include "request_builder.hpp"

#include "backends/contract.hpp"
#include "backends/credentials.hpp"

namespace karu {
namespace {

void apply_http_options(const ConfigSnapshot& config, std::string_view path,
                        PreparedRequest& request) {
    const bool follow_redirects = request.http.follow_redirects;
    request.http = config.http_options(path);
    request.http.follow_redirects = follow_redirects;
}

} // namespace

std::expected<PreparedRequest, RequestError>
RequestBuilder::prepare(const Locator& locator, std::uint64_t first, std::uint64_t length,
                        std::string_view region_hint, std::string_view if_match) {
    const Resolved& object = locator.resolved;
    if (length == 0 || first > UINT64_MAX - (length - 1))
        return std::unexpected(RequestError{KARU_ERR_RANGE, "HTTP range overflows"});

    if (object.backend == Backend::Http || object.backend == Backend::HuggingFace) {
        auto prepared = object.backend == Backend::Http
                            ? backends::prepare_http(config_, object, if_match)
                            : backends::prepare_hugging_face(config_, object, if_match);
        if (prepared)
            apply_http_options(config_, object.canonical_uri, *prepared);
        return prepared;
    }

    const backends::CloudProvider* provider = backends::cloud_provider(object.backend);
    if (provider == nullptr)
        return std::unexpected(RequestError{KARU_ERR_UNSUPPORTED, "unsupported request backend"});

    ProviderCredentials credentials;
    const ProviderCredentials* credentials_ptr = nullptr;
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
    if (!no_sign) {
        auto loaded =
            custom_provider
                ? credentials_.custom(config_, provider->credentials_kind, object.canonical_uri)
                : credentials_.native(config_, *provider, object.canonical_uri);
        if (!loaded)
            return std::unexpected(loaded.error());
        credentials = std::move(*loaded);
        credentials_ptr = &credentials;
    }

    backends::RequestContext request{config_, object,      credentials_ptr, first,
                                     length,  region_hint, if_match};
    auto prepared = provider->prepare_request(request);
    if (!prepared)
        return std::unexpected(prepared.error());
    if (auto headers =
            backends::add_gdal_headers(config_, object.canonical_uri, prepared->headers, true);
        !headers) {
        return std::unexpected(headers.error());
    }
    apply_http_options(config_, object.canonical_uri, *prepared);
    return prepared;
}

void RequestBuilder::invalidate_credentials(const Locator& locator) {
    const backends::CloudProvider* provider = backends::cloud_provider(locator.resolved.backend);
    if (provider == nullptr)
        return;
    const bool custom = config_.provider(provider->credentials_kind) != nullptr;
    credentials_.invalidate(config_, *provider, locator.resolved.canonical_uri, custom);
}

} // namespace karu
