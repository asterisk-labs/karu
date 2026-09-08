#include "request_builder.hpp"

#include "backends/contract.hpp"
#include "backends/credentials.hpp"

namespace karu {

std::expected<PreparedRequest, RequestError>
RequestBuilder::prepare(const Locator& locator, std::uint64_t first, std::uint64_t length,
                        std::string_view region_hint, std::string_view if_match) {
    const Resolved& object = locator.resolved;
    if (length == 0 || first > UINT64_MAX - (length - 1))
        return std::unexpected(RequestError{KARU_ERR_RANGE, "HTTP range overflows"});

    if (object.backend == Backend::Http)
        return backends::prepare_http(config_, object, if_match);
    if (object.backend == Backend::HuggingFace)
        return backends::prepare_hugging_face(config_, object, if_match);

    const backends::CloudProvider* provider = backends::cloud_provider(object.backend);
    if (provider == nullptr)
        return std::unexpected(RequestError{KARU_ERR_UNSUPPORTED, "unsupported request backend"});

    ProviderCredentials credentials;
    const ProviderCredentials* credentials_ptr = nullptr;
    const auto custom_provider = config_.provider(provider->credentials_kind);
    bool credentials_configured = custom_provider != nullptr;
    for (std::string_view option : provider->credential_options)
        credentials_configured =
            credentials_configured || config_.has_option(object.canonical_uri, option);

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
    return prepared;
}

} // namespace karu
