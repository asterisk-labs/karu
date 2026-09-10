#include "backends/contract.hpp"
#include "locator.hpp"
#include "test_support.hpp"

namespace karu::test {

void test_azure_request() {
    SECTION("Azure request signing");
    // "a2V5" is base64("key").
    karu::ConfigBuilder azure_builder(false);
    OK(azure_builder.set("AZURE_STORAGE_ACCOUNT", "account"));
    OK(azure_builder.set("AZURE_STORAGE_ACCESS_KEY", "a2V5"));
    karu::RequestBuilder azure_request_builder(must_freeze(azure_builder));
    karu::Locator azure{must_resolve("az://container/a b")};
    auto azure_request = azure_request_builder.prepare(azure, 3, 7);
    OK(azure_request.has_value());
    if (azure_request) {
        EQS(azure_request->url, "https://account.blob.core.windows.net/container/a%20b");
        EQS(azure_request->range, "bytes=3-9");
        OK(header(*azure_request, "Authorization").starts_with("SharedKey account:"));
        OK(!header(*azure_request, "x-ms-date").empty());
    }

    const ConfigSnapshot exact_config = must_freeze(azure_builder);
    const Resolved exact_object = must_resolve("az://container/a b");
    ProviderCredentials exact_credentials;
    exact_credentials.account_name = "account";
    exact_credentials.secret_access_key = "a2V5";
    const backends::RequestContext exact_context{
        exact_config, exact_object, &exact_credentials, "bytes=3-9", {}, "\"etag\"", 1'440'938'160};
    auto exact_request = backends::azure_provider().prepare_request(exact_context);
    OK(exact_request.has_value());
    if (exact_request) {
        EQS(header(*exact_request, "x-ms-date"), "Sun, 30 Aug 2015 12:36:00 GMT");
        EQS(header(*exact_request, "Authorization"),
            "SharedKey account:ATPHSo27zQ9aJ7DUXth+r77/Cii4diPaL3gbBGTR2JE=");
    }

    karu::ConfigBuilder anonymous_azure_builder(false);
    OK(anonymous_azure_builder.set(
        "AZURE_STORAGE_CONNECTION_STRING",
        "DefaultEndpointsProtocol=http;AccountName=dev;SharedAccessSignature=secret=1;"
        "EndpointSuffix=example.test"));
    OK(anonymous_azure_builder.set("AZURE_NO_SIGN_REQUEST", "YES"));
    karu::RequestBuilder anonymous_azure(must_freeze(anonymous_azure_builder));
    auto anonymous_request = anonymous_azure.prepare(azure, 0, 1);
    OK(anonymous_request.has_value());
    if (anonymous_request) {
        EQS(anonymous_request->url, "http://dev.blob.example.test/container/a%20b");
        OK(anonymous_request->headers.empty());
    }

    karu::ConfigBuilder ambiguous_identity(false);
    OK(ambiguous_identity.set("AZURE_IMDS_OBJECT_ID", "object"));
    OK(ambiguous_identity.set("AZURE_IMDS_CLIENT_ID", "client"));
    karu::RequestBuilder ambiguous_request_builder(must_freeze(ambiguous_identity));
    auto ambiguous = ambiguous_request_builder.prepare(azure, 0, 1);
    OK(!ambiguous);
    if (!ambiguous)
        EQ(ambiguous.error().status, KARU_ERR_CONFIG);
}

} // namespace karu::test
