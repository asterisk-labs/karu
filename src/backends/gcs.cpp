#include "../text.hpp"
#include "contract.hpp"
#include "crypto.hpp"
#include "gcs_credentials.hpp"
#include "url.hpp"

#include <array>
#include <ctime>
#include <openssl/evp.h>
#include <span>

namespace karu::backends {
namespace {

constexpr std::array<std::string_view, 16> CREDENTIAL_OPTIONS{"GS_ACCESS_KEY_ID",
                                                              "GS_SECRET_ACCESS_KEY",
                                                              "GS_OAUTH2_ACCESS_TOKEN",
                                                              "GS_OAUTH2_REFRESH_TOKEN",
                                                              "GS_OAUTH2_CLIENT_ID",
                                                              "GS_OAUTH2_CLIENT_SECRET",
                                                              "GS_OAUTH2_PRIVATE_KEY",
                                                              "GS_OAUTH2_PRIVATE_KEY_FILE",
                                                              "GS_OAUTH2_CLIENT_EMAIL",
                                                              "GS_OAUTH2_SCOPE",
                                                              "GOOGLE_APPLICATION_CREDENTIALS",
                                                              "CLOUDSDK_CONFIG",
                                                              "CPL_GS_CREDENTIALS_FILE",
                                                              "CPL_GCE_SKIP",
                                                              "CPL_MACHINE_IS_GCE",
                                                              "CPL_GCE_CREDENTIALS_URL"};

std::expected<PreparedRequest, RequestError>
prepare_gcs(const ConfigSnapshot& config, const Resolved& object, const ProviderCredentials* custom,
            std::uint64_t first, std::uint64_t length, std::string_view if_match) {
    static_cast<void>(first);
    static_cast<void>(length);
    const std::string& path = object.canonical_uri;
    std::string endpoint = config.option(path, "CPL_GS_ENDPOINT", "https://storage.googleapis.com");
    auto normalized_endpoint = http_endpoint(std::move(endpoint), "CPL_GS_ENDPOINT");
    if (!normalized_endpoint)
        return std::unexpected(normalized_endpoint.error());
    endpoint = std::move(*normalized_endpoint);
    auto endpoint_parts = split_url(endpoint);
    if (!endpoint_parts)
        return std::unexpected(endpoint_parts.error());
    if (!endpoint_parts->query.empty())
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, "CPL_GS_ENDPOINT cannot contain a query"});
    std::string url = append_object(endpoint, object.container, object.key);
    std::vector<Header> headers;
    if (!if_match.empty())
        headers.emplace_back("If-Match", if_match);
    const std::string user_project = config.option(path, "GS_USER_PROJECT");
    if (!user_project.empty())
        headers.emplace_back("x-goog-user-project", user_project);
    if (option_is_true(config.option(path, "GS_NO_SIGN_REQUEST", "NO")))
        return PreparedRequest{std::move(url), std::move(headers)};

    ProviderCredentials credentials;
    if (custom != nullptr) {
        credentials = *custom;
    } else {
        credentials.access_key_id = config.option(path, "GS_ACCESS_KEY_ID");
        credentials.secret_access_key = config.option(path, "GS_SECRET_ACCESS_KEY");
        credentials.bearer_token = config.option(path, "GS_OAUTH2_ACCESS_TOKEN");
    }
    if (!credentials.bearer_token.empty()) {
        headers.emplace_back("Authorization", "Bearer " + credentials.bearer_token);
        return PreparedRequest{std::move(url), std::move(headers)};
    }
    if (!credentials.access_key_id.empty() && !credentials.secret_access_key.empty()) {
        const std::string date = rfc7231_date(std::time(nullptr));
        const std::string canonical_headers =
            user_project.empty() ? "" : "x-goog-user-project:" + user_project + "\n";
        const std::string canonical = "GET\n\n\n" + date + "\n" + canonical_headers + "/" +
                                      object.container + "/" + encode_path(object.key);
        const auto key =
            std::span(reinterpret_cast<const unsigned char*>(credentials.secret_access_key.data()),
                      credentials.secret_access_key.size());
        headers.emplace_back("Date", date);
        headers.emplace_back("Authorization", "GOOG1 " + credentials.access_key_id + ":" +
                                                  base64(hmac(EVP_sha1(), key, canonical)));
        return PreparedRequest{std::move(url), std::move(headers)};
    }
    return std::unexpected(
        RequestError{KARU_ERR_CREDENTIALS,
                     path + ": no GCS credentials; set GS_OAUTH2_ACCESS_TOKEN, configure "
                            "GS_ACCESS_KEY_ID/GS_SECRET_ACCESS_KEY, install a custom provider, "
                            "or set GS_NO_SIGN_REQUEST=YES"});
}

} // namespace

const CloudProvider& gcs_provider() noexcept {
    static const CloudProvider provider{Backend::Gcs,
                                        KARU_CREDENTIALS_GCS,
                                        "GS_NO_SIGN_REQUEST",
                                        false,
                                        CREDENTIAL_OPTIONS,
                                        load_gcs_credentials,
                                        [](const RequestContext& request) {
                                            return prepare_gcs(request.config, request.object,
                                                               request.credentials, request.first,
                                                               request.length, request.if_match);
                                        }};
    return provider;
}

} // namespace karu::backends
