#include "karu/karu.h"
#include "test_support.hpp"

#include <cstdlib>
#include <string>

namespace karu::test {

void test_uri_identity() {
    SECTION("URI identity");
    auto local = must_resolve("file:///tmp/a.bin");
    EQ(local.backend, karu::Backend::File);
    EQS(local.target, "/tmp/a.bin");

    auto s3 = must_resolve("s3://bucket/a//b c.tif");
    EQ(s3.backend, karu::Backend::S3);
    EQS(s3.container, "bucket");
    EQS(s3.key, "a//b c.tif");
    EQS(s3.canonical_uri, "/vsis3/bucket/a//b c.tif");

    auto gcs = must_resolve("/vsigs/bucket/key");
    EQ(gcs.backend, karu::Backend::Gcs);
    EQS(gcs.canonical_uri, "/vsigs/bucket/key");

    auto azure = must_resolve("/vsiadls/container/a/b");
    EQ(azure.backend, karu::Backend::Azure);
    OK(azure.azure_dfs);

    auto source = must_resolve("source://account/product/a//b c.tif");
    EQ(source.backend, karu::Backend::Source);
    EQS(source.container, "account");
    EQS(source.key, "product/a//b c.tif");
    EQS(source.canonical_uri, "/vsisource/account/product/a//b c.tif");
    OK(!karu::resolve("source://account/product"));

    auto http = must_resolve("/vsicurl/https://example.test/a?x=1");
    EQ(http.backend, karu::Backend::Http);
    EQS(http.target, "https://example.test/a?x=1");

    OK(!karu::resolve("/vsis3_streaming/bucket/key"));
    OK(!karu::resolve("/vsicurl_streaming/https://example.test/a"));
    OK(!karu::resolve("/vsizip/archive.zip/file"));
}

void test_windows() {
    SECTION("windows");
    auto object = must_resolve("/vsisubfile/5_10,s3://bucket/key#bytes=100-199");
    EQ(object.window_offset, 105u);
    EQ(object.window_length, 10u);
    EQS(object.canonical_uri, "/vsis3/bucket/key");
    OK(karu::resolve("/vsisubfile/1000_-1,/tmp/a")->window_length == karu::TO_END);
    OK(!karu::resolve("https://example.test/a#bytes=10-4"));
    OK(!karu::resolve("/vsisubfile/1_2"));

    auto source =
        must_resolve("/vsisubfile/1048576_8388608,/vsisource/account/product/archive.cozip");
    EQ(source.backend, karu::Backend::Source);
    EQ(source.window_offset, 1048576u);
    EQ(source.window_length, 8388608u);
    EQS(source.canonical_uri, "/vsisource/account/product/archive.cozip");

    std::string nested = "/tmp/a";
    for (int depth = 0; depth < 16; ++depth)
        nested = "/vsisubfile/0_1," + nested;
    OK(karu::resolve(nested));
    nested = "/vsisubfile/0_1," + nested;
    OK(!karu::resolve(nested));
}

void test_config_precedence() {
    SECTION("configuration precedence");
    set_env("AWS_ENDPOINT_URL", "env-general.test");
    set_env("AWS_ENDPOINT_URL_S3", "env-s3.test");
    karu::ConfigBuilder builder(true);
    unset_env("AWS_ENDPOINT_URL");
    unset_env("AWS_ENDPOINT_URL_S3");

    auto snapshot = must_freeze(builder);
    EQS(snapshot.option("/vsis3/a/k", "AWS_S3_ENDPOINT"), "env-s3.test");

    OK(builder.set("AWS_ENDPOINT_URL", "explicit.test"));
    OK(builder.set_path("s3://a/", "AWS_ENDPOINT_URL_S3", "bucket.test"));
    OK(builder.set_path("/vsis3/a/private/", "AWS_S3_ENDPOINT", "private.test"));
    snapshot = must_freeze(builder);
    EQS(snapshot.option("/vsis3/b/k", "AWS_S3_ENDPOINT"), "explicit.test");
    EQS(snapshot.option("/vsis3/a/public/k", "AWS_S3_ENDPOINT"), "bucket.test");
    EQS(snapshot.option("s3://a/private/x", "AWS_S3_ENDPOINT"), "private.test");

    OK(builder.set("SOURCE_PROXY_URL", "https://proxy.source.test"));
    OK(builder.set_path("source://account/product/", "SOURCE_ENDPOINT",
                        "https://product.source.test"));
    snapshot = must_freeze(builder);
    EQS(snapshot.option("/vsisource/other/product/key", "SOURCE_ENDPOINT"),
        "https://proxy.source.test");
    EQS(snapshot.option("/vsisource/account/product/key", "SOURCE_ENDPOINT"),
        "https://product.source.test");

    OK(builder.set("AWS_DEFAULT_REGION", "eu-west-1"));
    OK(builder.set_path("/vsis3/a/", "AWS_DEFAULT_PROFILE", "bucket-profile"));
    OK(builder.set("CPL_AWS_CREDENTIALS_FILE", "/credentials"));
    snapshot = must_freeze(builder);
    EQS(snapshot.option("/vsis3/b/k", "AWS_REGION"), "eu-west-1");
    EQS(snapshot.option("/vsis3/a/k", "AWS_PROFILE"), "bucket-profile");
    EQS(snapshot.option("/vsis3/a/k", "AWS_SHARED_CREDENTIALS_FILE"), "/credentials");

    karu::ConfigBuilder vocabulary(false);
    OK(vocabulary.set("GS_OAUTH2_PRIVATE_KEY_FILE", "/key.pem"));
    OK(vocabulary.set("GS_OAUTH2_CLIENT_EMAIL", "reader@example.test"));
    OK(vocabulary.set("GS_OAUTH2_SCOPE", "scope"));
    OK(vocabulary.set("CPL_GS_CREDENTIALS_FILE", "/credentials.boto"));
    OK(vocabulary.set("CPL_MACHINE_IS_GCE", "NO"));
    OK(vocabulary.set("AZURE_IMDS_OBJECT_ID", "identity"));
    OK(vocabulary.set("SOURCE_PROFILE", "source-coop"));
    OK(vocabulary.set("GDAL_HTTP_VERSION", "2TLS"));
    OK(vocabulary.set("CURL_CA_BUNDLE", "/certificates.pem"));
    OK(vocabulary.set("GDAL_HTTP_PROXY", "http://proxy.example.test:8080"));
    OK(vocabulary.set("GDAL_HTTP_USERAGENT", "karu-test"));
    OK(vocabulary.freeze());

    RequestBuilder http_builder(must_freeze(vocabulary));
    auto http_request =
        http_builder.prepare(Locator{must_resolve("https://example.test/object")}, 0, 1);
    OK(http_request.has_value());
    if (http_request) {
        EQ(http_request->http.version, HttpVersion::Http2Tls);
        EQS(http_request->http.ca_bundle, "/certificates.pem");
        EQS(http_request->http.proxy, "http://proxy.example.test:8080");
        EQS(http_request->http.user_agent, "karu-test");
    }

    // A config is snapshotted by the client, so later builder mutations do
    // not mutate a running transport.
    karu_config* c_config = nullptr;
    karu_client* client = nullptr;
    EQ(karu_config_create_empty(&c_config), KARU_OK);
    EQ(karu_config_set_option(c_config, "KARU_CONCURRENCY", "7"), KARU_OK);
    EQ(karu_client_create(c_config, &client), KARU_OK);
    EQ(karu_config_set_option(c_config, "KARU_CONCURRENCY", "19"), KARU_OK);
    EQ(karu_client_concurrency(client), 7);
    karu_client_free(client);
    karu_config_free(c_config);

    karu::ConfigBuilder invalid(false);
    OK(!must_freeze(invalid).discover_default_credentials());
    OK(!invalid.set("AWS_REGOIN", "us-east-1"));
    OK(invalid.set("AWS_NO_SIGN_REQUEST", "perhaps"));
    OK(!invalid.freeze());
    OK(!invalid.set_path("/vsis3/", "KARU_CONCURRENCY", "2"));

    karu::ConfigBuilder invalid_http(false);
    OK(invalid_http.set("GDAL_HTTP_VERSION", "3"));
    OK(!invalid_http.freeze());

#ifdef _WIN32
    constexpr const char* home_name = "USERPROFILE";
#else
    constexpr const char* home_name = "HOME";
#endif
    const char* old_home_value = std::getenv(home_name);
    const std::string old_home = old_home_value == nullptr ? "" : old_home_value;
    set_env(home_name, "/snapshot-home");
    karu::ConfigBuilder home_builder(true);
    set_env(home_name, "/changed-home");
    auto home_snapshot = must_freeze(home_builder);
    OK(home_snapshot.discover_default_credentials());
    EQS(home_snapshot.expand_user_path("~"), "/snapshot-home");
    EQS(home_snapshot.default_aws_path("credentials"), "/snapshot-home/.aws/credentials");
    if (old_home_value == nullptr)
        unset_env(home_name);
    else
        set_env(home_name, old_home.c_str());
}

} // namespace karu::test
