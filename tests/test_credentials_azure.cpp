// Azure credential discovery and the Source Cooperative profile loader.
//
// The loopback fixture serves a fixed route table, so a case that needs to see
// the URL Azure built, the form it posted or the header it attached reads the
// fixture's own request log instead of an echo route. Two Azure entry points
// hardcode a live address (login.microsoftonline.com for the OAuth authority
// and 169.254.169.254 for IMDS); every case below either pins the authority to
// loopback or routes the IMDS probe through a loopback proxy, so a regression
// fails offline instead of dialling a real endpoint.
#include "backends/azure_credentials.hpp"
#include "backends/credentials.hpp"
#include "backends/source_credentials.hpp"
#include "config_options.hpp"
#include "credential_support.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <ctime>
#include <expected>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace karu::test {
namespace {

using Result = std::expected<ProviderCredentials, RequestError>;
using Option = std::pair<const char*, std::string>;

constexpr std::string_view azure_path = "/vsiaz/container/blob.bin";
constexpr std::string_view source_path = "/vsisource/account/product/object.bin";

// Nothing listens on the discard port, so a request that must not happen ends
// in a refused connection rather than a wait. credential_request() uses its own
// one to five second connect timeouts and ignores KARU_CONNECT_TIMEOUT.
constexpr const char* closed_endpoint = "http://127.0.0.1:9/identity";

ConfigSnapshot frozen(std::initializer_list<Option> options) {
    ConfigBuilder builder = empty_builder();
    for (const auto& [name, value] : options) {
        if (!builder.set(name, value.c_str()))
            fail(__LINE__, std::string("could not set ") + name);
    }
    return must_freeze(builder);
}

void expect_failure(int line, const Result& result, karu_status status, std::string_view message) {
    ++checks;
    if (result) {
        fail(line, "expected a failure, got credentials");
        return;
    }
    if (result.error().status != status) {
        fail(line, "expected status " + std::to_string(static_cast<int>(status)) + ", got " +
                       std::to_string(static_cast<int>(result.error().status)));
    }
    string_at(line, result.error().message, message, "error message");
}

// The libcurl text after the prefix is version specific, so only the prefix
// karu itself adds is an assertion.
void expect_transport_failure(int line, const Result& result) {
    ++checks;
    if (result) {
        fail(line, "expected a transport failure, got credentials");
        return;
    }
    if (result.error().status != KARU_ERR_CREDENTIALS ||
        !result.error().message.starts_with("credential endpoint: "))
        fail(line, "not a transport failure: '" + result.error().message + "'");
}

void expect_expiry_near(int line, std::int64_t actual, std::int64_t expected) {
    ++checks;
    const std::int64_t drift = actual - expected;
    if (drift < -5 || drift > 5) {
        fail(line, "expiry " + std::to_string(actual) + " is not within five seconds of " +
                       std::to_string(expected));
    }
}

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// Entries are json.dumps output, so quoting the whole field value anchors both
// ends of the match: a stray extra path segment or form parameter cannot slip
// past a substring search.
std::string logged_path(std::string_view value) {
    return "\"path\": \"" + std::string(value) + "\"";
}

std::string logged_body(std::string_view value) {
    return "\"body\": \"" + std::string(value) + "\"";
}

void expect_logged(int line, const std::string& log, std::string_view needle) {
    ++checks;
    if (log.find(needle) == std::string::npos)
        fail(line, "the fixture never saw " + std::string(needle));
}

void expect_not_logged(int line, const std::string& log, std::string_view needle) {
    ++checks;
    if (log.find(needle) != std::string::npos)
        fail(line, "the fixture saw " + std::string(needle) + " and should not have");
}

// load_azure_credentials builds "<authority>/<tenant>/oauth2/v2.0/token", which
// is not a route the fixture serves. Ending the authority in a query parameter
// moves the tenant segment into the query, so the path still resolves and the
// log still shows exactly what was built.
std::string oauth_authority(std::string_view route) {
    return fixture_url(route) + "?authority=";
}

// libcurl reads these straight from the environment whenever CURLOPT_PROXY is
// not set, and it applies them to loopback URLs as well - it has no implicit
// localhost bypass. A developer or CI image with a proxy exported would
// therefore push every fixture request, including the fabricated Azure client
// secrets in the OAuth bodies, at that proxy, stall on it instead of on
// loopback, and let the default IMDS probe escape to the real link-local
// address. The whole suite runs with them hidden.
inline constexpr std::array<const char*, 8> proxy_environment_names{
    "no_proxy",    "NO_PROXY",    "http_proxy", "HTTP_PROXY",
    "https_proxy", "HTTPS_PROXY", "all_proxy",  "ALL_PROXY"};

class HiddenProxyEnvironment {
  public:
    HiddenProxyEnvironment() {
        for (const char* name : proxy_environment_names)
            entries_.push_back(std::make_unique<ScopedEnvironment>(name, nullptr));
    }

    HiddenProxyEnvironment(const HiddenProxyEnvironment&) = delete;
    HiddenProxyEnvironment& operator=(const HiddenProxyEnvironment&) = delete;

  protected:
    std::vector<std::unique_ptr<ScopedEnvironment>> entries_;
};

// ConfigBuilder(true) snapshots every name in config_options, so the three
// cases that need the environment layer have to hide all of them first: a stray
// KARU_HTTP_VERSION or CURL_CA_BUNDLE would change the transport under test,
// and a developer's no_proxy entry would let the default IMDS probe escape the
// loopback proxy and reach the real link-local address.
class ScrubbedEnvironment : public HiddenProxyEnvironment {
  public:
    ScrubbedEnvironment() {
        config_options::for_each_environment([this](const char* name) { hide(name); });
    }

    void set(const char* name, const std::string& value) {
        entries_.push_back(std::make_unique<ScopedEnvironment>(name, value.c_str()));
    }

  private:
    void hide(const char* name) {
        entries_.push_back(std::make_unique<ScopedEnvironment>(name, nullptr));
    }
};

void azure_direct_credentials() {
    SECTION("azure direct credentials");

    // A connection string short-circuits everything else. The unreachable
    // identity endpoint is the assertion: if the early return regressed, the
    // call would fail instead of succeeding.
    {
        const auto config =
            frozen({{"AZURE_STORAGE_CONNECTION_STRING",
                     "DefaultEndpointsProtocol=http;AccountName=dev;AccountKey=a2V5"},
                    {"AZURE_STORAGE_ACCOUNT", "dev"},
                    {"IDENTITY_ENDPOINT", closed_endpoint}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->account_name, "dev");
            OK(credentials->secret_access_key.empty());
            OK(credentials->sas_token.empty());
            OK(credentials->bearer_token.empty());
        }
    }

    {
        const auto config = frozen({{"AZURE_STORAGE_ACCOUNT", "account"},
                                    {"AZURE_STORAGE_SAS_TOKEN", "sv=2021-08-06&sig=abc"},
                                    {"IDENTITY_ENDPOINT", closed_endpoint}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->sas_token, "sv=2021-08-06&sig=abc");
            EQS(credentials->account_name, "account");
            OK(credentials->bearer_token.empty());
        }
    }

    // A statically supplied bearer token carries no expiry.
    {
        const auto config = frozen(
            {{"AZURE_STORAGE_ACCESS_TOKEN", "static-bearer"}, {"IMDS_ENDPOINT", closed_endpoint}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->bearer_token, "static-bearer");
            EQ(credentials->expires_at, 0);
            OK(credentials->sas_token.empty());
        }
    }

    // A shared key wins over a fully configured service principal. The closed
    // authority keeps that regression offline instead of posting to Microsoft.
    {
        const auto config = frozen({{"AZURE_STORAGE_ACCOUNT", "account"},
                                    {"AZURE_STORAGE_ACCESS_KEY", "a2V5"},
                                    {"AZURE_TENANT_ID", "tenant"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_CLIENT_SECRET", "secret"},
                                    {"AZURE_AUTHORITY_HOST", "http://127.0.0.1:9"}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->secret_access_key, "a2V5");
            EQS(credentials->account_name, "account");
            OK(credentials->bearer_token.empty());
        }
    }

    // The gate that keeps an empty configuration from ever touching IMDS.
    // These two are the only cases whose correct behaviour is "no request at
    // all" on the IMDS path, so a regression of that gate would dial the
    // hardcoded 169.254.169.254 for real: a one second connect stall on a
    // developer machine, and live credentials on an Azure VM. The closed
    // loopback proxy is the offline guard, exactly as the unreachable identity
    // endpoint is above; it cannot change the outcome when the gate holds,
    // because no request is made.
    {
        const auto config = frozen({{"KARU_HTTP_PROXY", "http://127.0.0.1:9"}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            OK(credentials->account_name.empty());
            OK(credentials->secret_access_key.empty());
            OK(credentials->sas_token.empty());
            OK(credentials->bearer_token.empty());
            EQ(credentials->expires_at, 0);
        }
    }
    {
        const auto config = frozen(
            {{"AZURE_STORAGE_ACCOUNT", "account"}, {"KARU_HTTP_PROXY", "http://127.0.0.1:9"}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->account_name, "account");
            OK(credentials->bearer_token.empty());
            EQ(credentials->expires_at, 0);
        }
    }
}

void azure_service_principal() {
    SECTION("azure service principal");

    // The whole client-secret grant: the trailing slash trimmed off the
    // authority, the form-encoded tenant in the URL, the parameter order and
    // encoding of the body, and the Content-Type oauth_token appends.
    {
        reset_fixture_requests(__LINE__);
        const auto config = frozen({{"AZURE_TENANT_ID", "sp/one"},
                                    {"AZURE_CLIENT_ID", "app id"},
                                    {"AZURE_CLIENT_SECRET", "s3cr3t/+="},
                                    {"AZURE_STORAGE_SCOPE", "api://custom scope/.default"},
                                    {"AZURE_AUTHORITY_HOST", oauth_authority("/oauth/ok") + "/"}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->bearer_token, "oauth-access-token");
            expect_expiry_near(__LINE__, credentials->expires_at, now_seconds() + 3600);
        }
        const std::string log = fixture_request_body(__LINE__, "/_requests");
        // The tenant carries a slash, so the %2F is what proves form_encode was
        // applied to it: an unencoded "sp/one" would still reach this route.
        expect_logged(__LINE__, log,
                      logged_path("/oauth/ok?authority=/sp%2Fone/oauth2/v2.0/token"));
        expect_logged(__LINE__, log,
                      logged_body("client_id=app%20id&client_secret=s3cr3t%2F%2B%3D"
                                  "&scope=api%3A%2F%2Fcustom%20scope%2F.default"
                                  "&grant_type=client_credentials"));
        expect_logged(__LINE__, log, "\"content-type\": \"application/x-www-form-urlencoded\"");
        expect_logged(__LINE__, log, "\"method\": \"POST\"");
    }

    // A short lifetime is clamped up to the sixty second floor.
    {
        const auto config = frozen({{"AZURE_TENANT_ID", "clamp"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_CLIENT_SECRET", "secret"},
                                    {"AZURE_AUTHORITY_HOST", oauth_authority("/oauth/short")}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->bearer_token, "short-lived-token");
            expect_expiry_near(__LINE__, credentials->expires_at, now_seconds() + 60);
        }
    }

    // A response with no expires_in falls back to an hour. This is the only
    // fixture route that answers 200 with a token and no lifetime; it belongs
    // to the GCS metadata family but the OAuth reader only looks at the two
    // fields, so it is the one way to reach the fallback.
    {
        const auto config =
            frozen({{"AZURE_TENANT_ID", "no-lifetime"},
                    {"AZURE_CLIENT_ID", "app"},
                    {"AZURE_CLIENT_SECRET", "secret"},
                    {"AZURE_AUTHORITY_HOST", oauth_authority("/gcs/metadata/no-expires-in")}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->bearer_token, "gce-metadata-token");
            expect_expiry_near(__LINE__, credentials->expires_at, now_seconds() + 3600);
        }
    }

    // Workload identity: the federated assertion replaces the client secret and
    // the trailing newline in the token file must not reach the wire.
    {
        TempTree tree("azure_federated");
        const std::string token_file = tree.write("federated.jwt", "fed.token.value\n");
        reset_fixture_requests(__LINE__);
        const auto config = frozen({{"AZURE_TENANT_ID", "fed"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_FEDERATED_TOKEN_FILE", token_file},
                                    {"AZURE_AUTHORITY_HOST", oauth_authority("/oauth/ok")}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials)
            EQS(credentials->bearer_token, "oauth-access-token");
        const std::string log = fixture_request_body(__LINE__, "/_requests");
        expect_logged(__LINE__, log, logged_path("/oauth/ok?authority=/fed/oauth2/v2.0/token"));
        expect_logged(
            __LINE__, log,
            logged_body("client_id=app&client_assertion_type=urn%3Aietf%3Aparams%3Aoauth%3A"
                        "client-assertion-type%3Ajwt-bearer&client_assertion=fed.token.value"
                        "&scope=https%3A%2F%2Fstorage.azure.com%2F.default"
                        "&grant_type=client_credentials"));
    }

    {
        const auto config = frozen({{"AZURE_TENANT_ID", "denied"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_CLIENT_SECRET", "secret"},
                                    {"AZURE_AUTHORITY_HOST", oauth_authority("/oauth/denied")}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS, "credential endpoint returned HTTP 400");
    }

    {
        const auto config = frozen({{"AZURE_TENANT_ID", "no-token"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_CLIENT_SECRET", "secret"},
                                    {"AZURE_AUTHORITY_HOST", oauth_authority("/oauth/no-token")}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS, "credential response has no access_token");
    }

    {
        const auto config = frozen({{"AZURE_TENANT_ID", "tenant"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_CLIENT_SECRET", "secret"},
                                    {"AZURE_AUTHORITY_HOST", "http://127.0.0.1:9"}});
        expect_transport_failure(__LINE__, backends::load_azure_credentials(config, azure_path));
    }
}

void azure_federated_token_file_errors() {
    SECTION("azure federated token file");
    TempTree tree("azure_token_file");

    {
        const std::string missing = tree.absent("does-not-exist.jwt");
        const auto config = frozen({{"AZURE_TENANT_ID", "tenant"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_FEDERATED_TOKEN_FILE", missing},
                                    {"AZURE_AUTHORITY_HOST", "http://127.0.0.1:9"}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS,
                       "Azure federated token: cannot open '" + missing + "'");
    }

    // One byte over the one MiB reader cap. The tree deletes the file with
    // itself, so nothing large outlives the case.
    {
        const std::string huge = tree.write("huge.jwt", std::string((1u << 20) + 1, 'a'));
        const auto config = frozen({{"AZURE_TENANT_ID", "tenant"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_FEDERATED_TOKEN_FILE", huge},
                                    {"AZURE_AUTHORITY_HOST", "http://127.0.0.1:9"}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS, "Azure federated token: file exceeds 1 MiB");
    }
}

void azure_managed_identity_selectors() {
    SECTION("azure managed identity selectors");
    constexpr std::string_view conflict =
        "set only one Azure managed-identity selector: object ID, client ID, or resource ID";

    // The check must fire before any HTTP; the closed endpoint would otherwise
    // produce a transport message instead.
    {
        const auto config = frozen({{"AZURE_IMDS_OBJECT_ID", "object"},
                                    {"AZURE_IMDS_MSI_RES_ID", "/subscriptions/x"},
                                    {"IDENTITY_ENDPOINT", closed_endpoint}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CONFIG, conflict);
    }

    // AZURE_CLIENT_ID is inherited as the IMDS client selector, so a plain
    // client ID conflicts with an object ID.
    {
        const auto config = frozen({{"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_IMDS_OBJECT_ID", "object"},
                                    {"IDENTITY_ENDPOINT", closed_endpoint}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CONFIG, conflict);
    }
}

void azure_app_service_identity() {
    SECTION("azure app service identity");
    const std::string identity_ok = fixture_url("/azure/identity/ok");

    // Endpoint without a query, default resource, object selector, identity
    // header, and an expires_on the server computes per request.
    {
        reset_fixture_requests(__LINE__);
        const auto config = frozen({{"IDENTITY_ENDPOINT", identity_ok},
                                    {"IDENTITY_HEADER", "secret-header"},
                                    {"AZURE_IMDS_OBJECT_ID", "my object"}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->bearer_token, "azure-identity-token");
            expect_expiry_near(__LINE__, credentials->expires_at, now_seconds() + 3600);
        }
        const std::string log = fixture_request_body(__LINE__, "/_requests");
        expect_logged(__LINE__, log,
                      logged_path("/azure/identity/ok?api-version=2019-08-01"
                                  "&resource=https%3A%2F%2Fstorage.azure.com%2F"
                                  "&object_id=my%20object"));
        expect_logged(__LINE__, log, "\"x-identity-header\": \"secret-header\"");
    }

    // Endpoint that already carries a query, a resource override, the resource
    // ID selector, no identity header, and no expires_on in the response.
    {
        reset_fixture_requests(__LINE__);
        const auto config =
            frozen({{"IDENTITY_ENDPOINT", fixture_url("/azure/identity/no-expiry?existing=1")},
                    {"AZURE_IMDS_MSI_RES_ID", "/subscriptions/x/y"},
                    {"AZURE_STORAGE_RESOURCE", "https://custom.resource.test/"}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->bearer_token, "azure-identity-token");
            expect_expiry_near(__LINE__, credentials->expires_at, now_seconds() + 3600);
        }
        const std::string log = fixture_request_body(__LINE__, "/_requests");
        expect_logged(__LINE__, log,
                      logged_path("/azure/identity/no-expiry?existing=1&api-version=2019-08-01"
                                  "&resource=https%3A%2F%2Fcustom.resource.test%2F"
                                  "&msi_res_id=%2Fsubscriptions%2Fx%2Fy"));
        expect_not_logged(__LINE__, log, "x-identity-header");
    }

    // A tenant and client with no secret and no federated file is not a service
    // principal: it falls through to managed identity, and the client ID
    // becomes the selector. The closed authority keeps a regression offline.
    {
        reset_fixture_requests(__LINE__);
        const auto config = frozen({{"AZURE_TENANT_ID", "tenant"},
                                    {"AZURE_CLIENT_ID", "app"},
                                    {"AZURE_AUTHORITY_HOST", "http://127.0.0.1:9"},
                                    {"IDENTITY_ENDPOINT", identity_ok}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials)
            EQS(credentials->bearer_token, "azure-identity-token");
        expect_logged(__LINE__, fixture_request_body(__LINE__, "/_requests"),
                      logged_path("/azure/identity/ok?api-version=2019-08-01"
                                  "&resource=https%3A%2F%2Fstorage.azure.com%2F&client_id=app"));
    }

    // An explicit IMDS client ID overrides the AZURE_CLIENT_ID fallback, and
    // both names collapsing into one selector keeps the conflict count at one.
    {
        reset_fixture_requests(__LINE__);
        const auto config = frozen({{"AZURE_CLIENT_ID", "app-fallback"},
                                    {"AZURE_IMDS_CLIENT_ID", "imds-explicit"},
                                    {"IDENTITY_ENDPOINT", identity_ok}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->bearer_token, "azure-identity-token");
            expect_expiry_near(__LINE__, credentials->expires_at, now_seconds() + 3600);
        }
        expect_logged(__LINE__, fixture_request_body(__LINE__, "/_requests"),
                      logged_path("/azure/identity/ok?api-version=2019-08-01"
                                  "&resource=https%3A%2F%2Fstorage.azure.com%2F"
                                  "&client_id=imds-explicit"));
    }

    // A tenant and secret without a client ID is not a service principal
    // either, and leaves no selector at all on the identity URL.
    {
        reset_fixture_requests(__LINE__);
        const auto config = frozen({{"AZURE_TENANT_ID", "tenant"},
                                    {"AZURE_CLIENT_SECRET", "secret"},
                                    {"AZURE_AUTHORITY_HOST", "http://127.0.0.1:9"},
                                    {"IDENTITY_ENDPOINT", identity_ok}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials)
            EQS(credentials->bearer_token, "azure-identity-token");
        expect_logged(__LINE__, fixture_request_body(__LINE__, "/_requests"),
                      logged_path("/azure/identity/ok?api-version=2019-08-01"
                                  "&resource=https%3A%2F%2Fstorage.azure.com%2F"));
    }

    {
        const auto config = frozen({{"IDENTITY_ENDPOINT", closed_endpoint}});
        expect_transport_failure(__LINE__, backends::load_azure_credentials(config, azure_path));
    }

    {
        const auto config = frozen({{"IDENTITY_ENDPOINT", fixture_url("/azure/identity/denied")}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS, "Azure App Service identity returned HTTP 403");
    }

    {
        const auto config =
            frozen({{"IDENTITY_ENDPOINT", fixture_url("/azure/identity/no-token")}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS,
                       "Azure App Service identity response has no access token");
    }

    // Header injection through IDENTITY_HEADER is refused before a connection
    // is opened; the closed endpoint would otherwise report a transport error.
    {
        const auto config = frozen(
            {{"IDENTITY_ENDPOINT", closed_endpoint}, {"IDENTITY_HEADER", "good\r\nX-Injected: 1"}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS, "credential header is invalid");
    }
}

void azure_imds_endpoint() {
    SECTION("azure configured imds endpoint");

    // A configured IMDS endpoint uses the older api-version and carries the
    // Metadata header that a real instance metadata service demands.
    {
        reset_fixture_requests(__LINE__);
        const auto config = frozen({{"IMDS_ENDPOINT", fixture_url("/azure/identity/ok")}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->bearer_token, "azure-identity-token");
            expect_expiry_near(__LINE__, credentials->expires_at, now_seconds() + 3600);
        }
        const std::string log = fixture_request_body(__LINE__, "/_requests");
        expect_logged(__LINE__, log,
                      logged_path("/azure/identity/ok?api-version=2018-02-01"
                                  "&resource=https%3A%2F%2Fstorage.azure.com%2F"));
        expect_logged(__LINE__, log, "\"metadata\": \"true\"");
    }

    {
        reset_fixture_requests(__LINE__);
        const auto config = frozen({{"IMDS_ENDPOINT", fixture_url("/azure/identity/ok?probe=1")},
                                    {"AZURE_IMDS_MSI_RES_ID", "/subscriptions/x/y"},
                                    {"AZURE_STORAGE_RESOURCE", "https://custom.resource.test/"}});
        auto credentials = backends::load_azure_credentials(config, azure_path);
        OK(credentials.has_value());
        if (credentials)
            EQS(credentials->bearer_token, "azure-identity-token");
        expect_logged(__LINE__, fixture_request_body(__LINE__, "/_requests"),
                      logged_path("/azure/identity/ok?probe=1&api-version=2018-02-01"
                                  "&resource=https%3A%2F%2Fcustom.resource.test%2F"
                                  "&msi_res_id=%2Fsubscriptions%2Fx%2Fy"));
    }

    // An endpoint the caller configured explicitly is never silently ignored:
    // every one of these three failures propagates.
    {
        const auto config = frozen({{"IMDS_ENDPOINT", closed_endpoint}});
        expect_transport_failure(__LINE__, backends::load_azure_credentials(config, azure_path));
    }

    {
        const auto config = frozen({{"IMDS_ENDPOINT", fixture_url("/azure/identity/denied")}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS, "Azure managed identity endpoint returned HTTP 403");
    }

    {
        const auto config = frozen({{"IMDS_ENDPOINT", fixture_url("/azure/identity/no-token")}});
        expect_failure(__LINE__, backends::load_azure_credentials(config, azure_path),
                       KARU_ERR_CREDENTIALS,
                       "Azure managed identity endpoint response has no access token");
    }
}

void azure_default_imds_endpoint() {
    SECTION("azure default imds endpoint");

    // The default endpoint is the hardcoded link-local address, which no test
    // may contact. Pointing CURLOPT_PROXY at loopback means libcurl never
    // resolves or reaches 169.254.169.254, and discover_default_credentials
    // needs the environment layer, hence ConfigBuilder(true) over a scrubbed
    // environment.
    const auto probe_through_proxy = [](const std::string& proxy) {
        ScrubbedEnvironment environment;
        ConfigBuilder builder(true);
        if (!builder.set("KARU_HTTP_PROXY", proxy.c_str()))
            fail(__LINE__, "could not set KARU_HTTP_PROXY");
        return backends::load_azure_credentials(must_freeze(builder), azure_path);
    };

    // A probe that cannot connect is swallowed, because the caller never asked
    // for this endpoint.
    {
        auto credentials = probe_through_proxy("http://127.0.0.1:9");
        OK(credentials.has_value());
        if (credentials) {
            OK(credentials->bearer_token.empty());
            OK(credentials->account_name.empty());
            EQ(credentials->expires_at, 0);
        }
    }

    // So is a non-2xx: the fixture has no route for the link-local path and
    // answers 404, which must still fall through to the empty credentials.
    {
        reset_fixture_requests(__LINE__);
        auto credentials = probe_through_proxy(fixture_url(""));
        OK(credentials.has_value());
        if (credentials) {
            OK(credentials->bearer_token.empty());
            EQ(credentials->expires_at, 0);
        }
        expect_logged(__LINE__, fixture_request_body(__LINE__, "/_requests"),
                      logged_path("http://169.254.169.254/metadata/identity/oauth2/token"
                                  "?api-version=2018-02-01"
                                  "&resource=https%3A%2F%2Fstorage.azure.com%2F"));
    }
}

void source_direct_credentials() {
    SECTION("source direct credentials");

    // The unreadable profile path is the assertion that direct keys return
    // before the profile loader runs.
    {
        const auto config = frozen({{"SOURCE_ACCESS_KEY_ID", "SOURCEKEY"},
                                    {"SOURCE_SECRET_ACCESS_KEY", "source-secret"},
                                    {"SOURCE_SESSION_TOKEN", "source-session"},
                                    {"SOURCE_SHARED_CREDENTIALS_FILE", "/nonexistent/never.ini"}});
        auto credentials = backends::load_source_credentials(config, source_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->access_key_id, "SOURCEKEY");
            EQS(credentials->secret_access_key, "source-secret");
            EQS(credentials->session_token, "source-session");
            OK(credentials->region.empty());
        }
    }

    constexpr std::string_view together =
        "/vsisource/account/product/object.bin: Source access key and secret must be set together";
    {
        const auto config = frozen({{"SOURCE_ACCESS_KEY_ID", "SOURCEKEY"}});
        expect_failure(__LINE__, backends::load_source_credentials(config, source_path),
                       KARU_ERR_CREDENTIALS, together);
    }
    {
        const auto config = frozen({{"SOURCE_SECRET_ACCESS_KEY", "source-secret"}});
        expect_failure(__LINE__, backends::load_source_credentials(config, source_path),
                       KARU_ERR_CREDENTIALS, together);
    }
}

void source_profile_files() {
    SECTION("source profile files");
    TempTree tree("source_files");

    // A pinned file that does not exist is an error, once per file option. The
    // message carries no /vsisource/ prefix: it comes from the profile reader.
    {
        const std::string config_file =
            tree.write("config.ini", "[profile source-coop]\nregion = us-west-2\n");
        const std::string missing = tree.absent("absent_credentials.ini");
        const auto config = frozen(
            {{"SOURCE_CONFIG_FILE", config_file}, {"SOURCE_SHARED_CREDENTIALS_FILE", missing}});
        expect_failure(__LINE__, backends::load_source_credentials(config, source_path),
                       KARU_ERR_CREDENTIALS,
                       "Source shared credentials file does not exist: '" + missing + "'");
    }
    {
        const std::string credentials_file = tree.write(
            "credentials.ini", "[source-coop]\naws_access_key_id = K\naws_secret_access_key = s\n");
        const std::string missing = tree.absent("absent_config.ini");
        const auto config = frozen({{"SOURCE_CONFIG_FILE", missing},
                                    {"SOURCE_SHARED_CREDENTIALS_FILE", credentials_file}});
        expect_failure(__LINE__, backends::load_source_credentials(config, source_path),
                       KARU_ERR_CREDENTIALS,
                       "Source config file does not exist: '" + missing + "'");
    }

    // The default profile name is source-coop, and a lone session token does
    // not count as direct credentials.
    {
        const std::string config_file =
            tree.write("other_config.ini", "[profile other]\naws_access_key_id = OTHER\n");
        const std::string credentials_file =
            tree.write("other_credentials.ini", "[other]\naws_secret_access_key = other-secret\n");
        const auto config = frozen({{"SOURCE_CONFIG_FILE", config_file},
                                    {"SOURCE_SHARED_CREDENTIALS_FILE", credentials_file},
                                    {"SOURCE_SESSION_TOKEN", "orphan-session"}});
        expect_failure(__LINE__, backends::load_source_credentials(config, source_path),
                       KARU_ERR_CREDENTIALS,
                       "/vsisource/account/product/object.bin: Source profile 'source-coop' "
                       "was not found");
    }

    // The section exists but yields no key pair.
    {
        const std::string config_file = tree.write("empty_config.ini", "# nothing here\n");
        const std::string credentials_file =
            tree.write("region_only.ini", "[source-coop]\nregion = us-east-1\n");
        const auto config = frozen({{"SOURCE_CONFIG_FILE", config_file},
                                    {"SOURCE_SHARED_CREDENTIALS_FILE", credentials_file}});
        expect_failure(__LINE__, backends::load_source_credentials(config, source_path),
                       KARU_ERR_CREDENTIALS,
                       "/vsisource/account/product/object.bin: Source profile 'source-coop' "
                       "did not provide credentials");
    }

    // The two files merge, with the shared credentials file winning. The config
    // file names the section "profile team" while the credentials file names it
    // "team", and the leading junk lines exercise the parser's skip arms.
    {
        const std::string config_file =
            tree.write("merge_config.ini", "orphan_key = ignored\n"
                                           "no_separator_line\n"
                                           "; semicolon comment\n"
                                           "# hash comment\n"
                                           "[profile team]\n"
                                           "region = eu-west-1\n"
                                           "aws_access_key_id = STALEKEY\n");
        const std::string credentials_file =
            tree.write("merge_credentials.ini", "[team]\n"
                                                "aws_access_key_id = FRESHKEY\n"
                                                "aws_secret_access_key = fresh-secret\n"
                                                "aws_session_token = fresh-session\n");
        const auto config = frozen({{"SOURCE_PROFILE", "team"},
                                    {"SOURCE_CONFIG_FILE", config_file},
                                    {"SOURCE_SHARED_CREDENTIALS_FILE", credentials_file}});
        auto credentials = backends::load_source_credentials(config, source_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->access_key_id, "FRESHKEY");
            EQS(credentials->secret_access_key, "fresh-secret");
            EQS(credentials->session_token, "fresh-session");
            EQS(credentials->region, "eu-west-1");
        }
    }

    // The default profile is the one section the config file spells without the
    // "profile " prefix.
    {
        const std::string config_file =
            tree.write("default_config.ini", "[default]\n"
                                             "aws_access_key_id = DEFKEY\n"
                                             "aws_secret_access_key = def-secret\n"
                                             "region = us-east-2\n");
        const std::string credentials_file = tree.write("default_credentials.ini", "");
        const auto config = frozen({{"SOURCE_PROFILE", "default"},
                                    {"SOURCE_CONFIG_FILE", config_file},
                                    {"SOURCE_SHARED_CREDENTIALS_FILE", credentials_file}});
        auto credentials = backends::load_source_credentials(config, source_path);
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->access_key_id, "DEFKEY");
            EQS(credentials->secret_access_key, "def-secret");
            EQS(credentials->region, "us-east-2");
        }
    }
}

void source_profile_shapes() {
    SECTION("source profile shapes");
    TempTree tree("source_shapes");
    const std::string config_file = tree.write("config.ini", "");

    // Each profile shape karu deliberately does not implement, and each half of
    // the two conditions that reject them.
    const auto rejected = [&](int line, std::string_view body, std::string_view message) {
        const std::string credentials_file = tree.write("credentials.ini", body);
        const auto config = frozen({{"SOURCE_CONFIG_FILE", config_file},
                                    {"SOURCE_SHARED_CREDENTIALS_FILE", credentials_file}});
        expect_failure(line, backends::load_source_credentials(config, source_path),
                       KARU_ERR_CREDENTIALS, message);
    };
    constexpr std::string_view assume_role =
        "Source AssumeRole profiles require a custom credential provider";
    constexpr std::string_view identity_center =
        "Source IAM Identity Center profiles require a custom credential provider";

    rejected(__LINE__,
             "[source-coop]\nrole_arn = arn:aws:iam::123456789012:role/example\n"
             "source_profile = base\n",
             assume_role);
    // role_arn and source_profile together only ever take the first arm of the
    // three-way ||, so source_profile needs its own profile to be tested at all.
    rejected(__LINE__, "[source-coop]\nsource_profile = base\n", assume_role);
    rejected(__LINE__, "[source-coop]\nweb_identity_token_file = token.jwt\n", assume_role);
    rejected(__LINE__, "[source-coop]\nsso_session = corp\n", identity_center);
    rejected(__LINE__, "[source-coop]\nsso_start_url = https://example.awsapps.com/start\n",
             identity_center);

    // Half a key pair inside a profile is a different layer, and a different
    // message, from half a key pair in the environment.
    constexpr std::string_view together =
        "Source profile access key and secret must be set together";
    rejected(__LINE__, "[source-coop]\naws_access_key_id = HALFKEY\n", together);
    rejected(__LINE__, "[source-coop]\naws_secret_access_key = half-secret\n", together);
}

void source_credential_process() {
    SECTION("source credential_process");
    TempTree tree("source_process");
    const std::string config_file = tree.write("config.ini", "");

    const auto with_helper = [&](std::string_view mode) {
        const std::string credentials_file =
            tree.write("credentials.ini",
                       std::string("[source-coop]\nregion = eu-central-1\n") +
                           "credential_process = " + credential_process_command(mode) + "\n");
        const auto config = frozen({{"SOURCE_CONFIG_FILE", config_file},
                                    {"SOURCE_SHARED_CREDENTIALS_FILE", credentials_file}});
        return backends::load_source_credentials(config, source_path);
    };

    // The helper reports its session token in the SessionToken field, which is
    // the fallback the Token field takes precedence over.
    {
        auto credentials = with_helper("ok");
        OK(credentials.has_value());
        if (credentials) {
            EQS(credentials->access_key_id, "AKIAHELPER");
            EQS(credentials->secret_access_key, "helper-secret");
            EQS(credentials->session_token, "helper-token");
            EQS(credentials->region, "eu-central-1");
            expect_expiry_near(__LINE__, credentials->expires_at, now_seconds() + 3600);
        }
    }

    expect_failure(__LINE__, with_helper("fail"), KARU_ERR_CREDENTIALS,
                   "Source credential_process exited with status 3");
    expect_failure(__LINE__, with_helper("incomplete"), KARU_ERR_CREDENTIALS,
                   "Source credential_process response is incomplete");
    // The helper floods two MiB; the reader stops and kills it at one.
    expect_failure(__LINE__, with_helper("flood"), KARU_ERR_CREDENTIALS,
                   "Source credential_process returned more than 1 MiB");
}

void source_default_profile_paths() {
    SECTION("source default profile paths");

    // With no file options set the loader falls back to ~/.aws. Only the
    // environment layer populates the home directory, so this is the one source
    // case that needs ConfigBuilder(true) - over a scrubbed environment and a
    // home directory inside the temporary tree.
    TempTree tree("source_home");
    const std::string home = tree.absent("home");
    static_cast<void>(tree.write(
        "home/.aws/credentials",
        "[source-coop]\naws_access_key_id = HOMEKEY\naws_secret_access_key = home-secret\n"));

    ScrubbedEnvironment environment;
    environment.set("HOME", home);
    environment.set("USERPROFILE", home);
    ConfigBuilder builder(true);
    auto credentials = backends::load_source_credentials(must_freeze(builder), source_path);
    OK(credentials.has_value());
    if (credentials) {
        EQS(credentials->access_key_id, "HOMEKEY");
        EQS(credentials->secret_access_key, "home-secret");
    }
}

} // namespace

void test_azure_credentials_loader() {
    const HiddenProxyEnvironment hidden_proxies;
    azure_direct_credentials();
    azure_service_principal();
    azure_federated_token_file_errors();
    azure_managed_identity_selectors();
    azure_app_service_identity();
    azure_imds_endpoint();
    azure_default_imds_endpoint();
}

void test_source_credentials_loader() {
    const HiddenProxyEnvironment hidden_proxies;
    source_direct_credentials();
    source_profile_files();
    source_profile_shapes();
    source_credential_process();
    source_default_profile_paths();
}

} // namespace karu::test
