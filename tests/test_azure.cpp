#include "backends/contract.hpp"
#include "locator.hpp"
#include "test_support.hpp"

#include <array>

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

    // Use keys at and above SHA-256's 64-byte HMAC block size. Short keys with
    // extra trailing zero bytes are HMAC-equivalent, so they cannot expose a
    // decoder that forgets to remove base64 padding.
    struct PaddedKeyCase {
        const char* encoded;
        const char* authorization;
    };
    constexpr std::array<PaddedKeyCase, 2> padded_keys{{
        {"a2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2traw==",
         "SharedKey account:wgCHM5hctsmdLxixnafE0zEgKvF9zyjUOaSEwFIl3ig="},
        {"a2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2s=",
         "SharedKey account:fSLuNynDfl8We3K3JtneyqYamNRzwJhyxngGFiZIo5I="},
    }};
    for (const PaddedKeyCase& padded : padded_keys) {
        ProviderCredentials padded_credentials;
        padded_credentials.account_name = "account";
        padded_credentials.secret_access_key = padded.encoded;
        const backends::RequestContext padded_context{
            exact_config, exact_object, &padded_credentials, "bytes=3-9",
            {},           "\"etag\"",   1'440'938'160};
        auto padded_request = backends::azure_provider().prepare_request(padded_context);
        OK(padded_request.has_value());
        if (padded_request)
            EQS(header(*padded_request, "Authorization"), padded.authorization);
    }

    // With no AZURE_STORAGE_ENDPOINT the connection string supplies the host,
    // and Data Lake paths read a different key than blob paths.
    karu::ConfigBuilder blob_endpoint(false);
    OK(blob_endpoint.set("AZURE_STORAGE_CONNECTION_STRING",
                         "AccountName=dev;AccountKey=a2V5;BlobEndpoint=https://blob.example.test"));
    karu::RequestBuilder blob_request_builder(must_freeze(blob_endpoint));
    auto blob_request = blob_request_builder.prepare(azure, 0, 1);
    OK(blob_request.has_value());
    if (blob_request)
        EQS(blob_request->url, "https://blob.example.test/container/a%20b");

    karu::ConfigBuilder dfs_endpoint(false);
    OK(dfs_endpoint.set("AZURE_STORAGE_CONNECTION_STRING",
                        "AccountName=dev;AccountKey=a2V5;DfsEndpoint=https://dfs.example.test"));
    karu::RequestBuilder dfs_request_builder(must_freeze(dfs_endpoint));
    karu::Locator adls{must_resolve("abfs://container/a b")};
    auto dfs_request = dfs_request_builder.prepare(adls, 0, 1);
    OK(dfs_request.has_value());
    if (dfs_request)
        EQS(dfs_request->url, "https://dfs.example.test/container/a%20b");

    // A query on the endpoint would collide with the SAS token appended below.
    karu::ConfigBuilder queried_endpoint(false);
    OK(queried_endpoint.set("AZURE_STORAGE_ACCOUNT", "account"));
    OK(queried_endpoint.set("AZURE_STORAGE_ACCESS_KEY", "a2V5"));
    OK(queried_endpoint.set("AZURE_STORAGE_ENDPOINT", "https://host.example.test/?prefix=x"));
    karu::RequestBuilder queried_request_builder(must_freeze(queried_endpoint));
    auto queried = queried_request_builder.prepare(azure, 0, 1);
    OK(!queried);
    if (!queried)
        EQ(queried.error().status, KARU_ERR_CONFIG);

    // A SAS token is pasted onto the URL, and the leading separators callers
    // copy out of the portal have to be stripped rather than doubled.
    for (const char* sas : {"sv=2021&sig=abc", "?sv=2021&sig=abc", "&sv=2021&sig=abc"}) {
        karu::ConfigBuilder sas_builder(false);
        OK(sas_builder.set("AZURE_STORAGE_ACCOUNT", "account"));
        OK(sas_builder.set("AZURE_STORAGE_SAS_TOKEN", sas));
        karu::RequestBuilder sas_request_builder(must_freeze(sas_builder));
        auto sas_request = sas_request_builder.prepare(azure, 0, 1);
        OK(sas_request.has_value());
        if (sas_request) {
            EQS(sas_request->url,
                "https://account.blob.core.windows.net/container/a%20b?sv=2021&sig=abc");
            OK(sas_request->headers.empty());
            OK(!sas_request->http.follow_redirects);
        }
    }
}

} // namespace karu::test
