// Pure surfaces that no other suite reaches: the public status table, VSI path
// canonicalization for every scheme, the configuration builder's rejections,
// and the transient-error classification behind the retry decision.
#include "backends/url.hpp"
#include "config.hpp"
#include "runtime/http_response.hpp"
#include "test_support.hpp"
#include "text.hpp"
#include "uri.hpp"

#include <array>
#include <string>

namespace karu::test {
namespace {

void status_table() {
    // Every enumerator must name itself. A missing case would fall through to
    // the shared "unknown status" text instead.
    constexpr std::array<karu_status, 17> all{KARU_OK,
                                              KARU_END,
                                              KARU_TIMEOUT,
                                              KARU_ERR_URI,
                                              KARU_ERR_UNSUPPORTED,
                                              KARU_ERR_INVALID,
                                              KARU_ERR_NOMEM,
                                              KARU_ERR_IO,
                                              KARU_ERR_NETWORK,
                                              KARU_ERR_HTTP,
                                              KARU_ERR_RANGE,
                                              KARU_ERR_AUTH,
                                              KARU_ERR_CANCELLED,
                                              KARU_ERR_CONFIG,
                                              KARU_ERR_CREDENTIALS,
                                              KARU_ERR_NOT_FOUND,
                                              KARU_ERR_PRECONDITION};
    for (const karu_status status : all) {
        const std::string text = karu_status_string(status);
        OK(!text.empty());
        if (text == "unknown status")
            fail(__LINE__, "status " + std::to_string(static_cast<int>(status)) + " has no name");
    }
    // Distinct statuses must not share a description.
    for (std::size_t left = 0; left < all.size(); ++left) {
        for (std::size_t right = left + 1; right < all.size(); ++right) {
            if (std::string(karu_status_string(all[left])) == karu_status_string(all[right]))
                fail(__LINE__, "two statuses share one description");
        }
    }
    // A value inside the enumeration's range but not one of its enumerators.
    // Anything outside that range would be undefined behaviour to load, which
    // UBSan reports, so this cannot be spelled with an arbitrary integer.
    EQS(karu_status_string(static_cast<karu_status>(3)), "unknown status");
}

void vsi_paths() {
    // Every URI scheme karu accepts has to fold onto its VSI spelling, because
    // path-specific options and credential scopes are matched after folding.
    EQS(canonical_vsi_path("s3://bucket/key"), "/vsis3/bucket/key");
    EQS(canonical_vsi_path("gs://bucket/key"), "/vsigs/bucket/key");
    EQS(canonical_vsi_path("az://container/blob"), "/vsiaz/container/blob");
    EQS(canonical_vsi_path("abfs://container/blob"), "/vsiadls/container/blob");
    EQS(canonical_vsi_path("hf://datasets/org/repo/file"), "/vsihf/datasets/org/repo/file");
    EQS(canonical_vsi_path("source://account/product/key"), "/vsisource/account/product/key");
    // Anything already in VSI form, or not a known scheme, passes through.
    EQS(canonical_vsi_path("/vsis3/bucket/key"), "/vsis3/bucket/key");
    EQS(canonical_vsi_path("https://host/object"), "https://host/object");
    EQS(canonical_vsi_path("/local/path"), "/local/path");
    EQS(canonical_vsi_path(""), "");

    // A path option set through a URI must match the VSI form of the same
    // object, for every scheme. The option itself is incidental; what is under
    // test is that both spellings land on one rule.
    struct Pair {
        const char* uri;
        const char* object;
    };
    constexpr std::array<Pair, 6> pairs{{{"s3://bucket", "s3://bucket/key"},
                                         {"gs://bucket", "gs://bucket/key"},
                                         {"az://container", "az://container/blob"},
                                         {"abfs://container", "abfs://container/blob"},
                                         {"hf://datasets/org", "hf://datasets/org/repo/file"},
                                         {"source://account", "source://account/product/key"}}};
    for (const Pair& pair : pairs) {
        ConfigBuilder builder(false);
        OK(builder.set_path(pair.uri, "AWS_REGION", "eu-central-1").has_value());
        const ConfigSnapshot snapshot = must_freeze(builder);
        EQS(snapshot.option(pair.object, "AWS_REGION"), "eu-central-1");
        EQS(snapshot.option(canonical_vsi_path(pair.object), "AWS_REGION"), "eu-central-1");
    }
}

void prefix_matching() {
    OK(path_prefix_matches("/vsis3/data/a", "/vsis3/data"));
    OK(path_prefix_matches("/vsis3/data", "/vsis3/data"));
    OK(path_prefix_matches("/vsis3/data/a", "/vsis3/data/"));
    // A rule for "data" must not capture "data-private".
    OK(!path_prefix_matches("/vsis3/data-private/a", "/vsis3/data"));
    OK(!path_prefix_matches("/vsis3/dat", "/vsis3/data"));

    OK(option_is_true("YES"));
    OK(option_is_true("yes"));
    OK(option_is_true("True"));
    OK(option_is_true("on"));
    OK(option_is_true("1"));
    OK(!option_is_true("NO"));
    OK(!option_is_true(""));
    OK(!option_is_true("2"));
    OK(!option_is_true("yess"));
}

void builder_rejections() {
    ConfigBuilder builder(false);
    OK(!builder.set("", "value").has_value());
    OK(!builder.set("KARU_NOT_AN_OPTION", "value").has_value());
    OK(!builder.set_path("", "AWS_REGION", "us-east-1").has_value());
    OK(!builder.set_path("/vsis3/bucket", "", "x").has_value());
    OK(!builder.set_path("/vsis3/bucket", "KARU_NOT_AN_OPTION", "x").has_value());
    // Client-wide options cannot be scoped to a path.
    OK(!builder.set_path("/vsis3/bucket", "KARU_CONCURRENCY", "8").has_value());
    OK(!builder.set_provider(static_cast<karu_credentials_kind>(5), nullptr, nullptr, nullptr)
            .has_value());
    // Clearing a provider must not carry user data.
    int marker = 0;
    OK(!builder.set_provider(KARU_CREDENTIALS_AWS, nullptr, &marker, nullptr).has_value());
    OK(builder.set_provider(KARU_CREDENTIALS_AWS, nullptr, nullptr, nullptr).has_value());

    // Removing an option restores the layer underneath it.
    OK(builder.set("AWS_REGION", "eu-west-1").has_value());
    EQS(must_freeze(builder).option("/vsis3/b/k", "AWS_REGION", "fallback"), "eu-west-1");
    OK(builder.set("AWS_REGION", nullptr).has_value());
    EQS(must_freeze(builder).option("/vsis3/b/k", "AWS_REGION", "fallback"), "fallback");
    OK(builder.set_path("/vsis3/b", "AWS_REGION", "ap-south-1").has_value());
    EQS(must_freeze(builder).option("/vsis3/b/k", "AWS_REGION", "fallback"), "ap-south-1");
    OK(builder.set_path("/vsis3/b", "AWS_REGION", nullptr).has_value());
    EQS(must_freeze(builder).option("/vsis3/b/k", "AWS_REGION", "fallback"), "fallback");

    // Rejected values are reported when the snapshot is frozen, not when set.
    ConfigBuilder invalid(false);
    OK(invalid.set("AWS_NO_SIGN_REQUEST", "perhaps").has_value());
    OK(!invalid.freeze().has_value());
    ConfigBuilder scoped(false);
    OK(scoped.set_path("/vsis3/b", "AWS_REQUEST_PAYER", "someone-else").has_value());
    OK(!scoped.freeze().has_value());
    ConfigBuilder version(false);
    OK(version.set("KARU_HTTP_VERSION", "3").has_value());
    OK(!version.freeze().has_value());
    ConfigBuilder timeout(false);
    OK(timeout.set("AWS_METADATA_SERVICE_TIMEOUT", "0").has_value());
    OK(!timeout.freeze().has_value());
    ConfigBuilder concurrency(false);
    OK(concurrency.set("KARU_CONCURRENCY", "0").has_value());
    OK(!concurrency.freeze().has_value());
    ConfigBuilder attempts(false);
    OK(attempts.set("KARU_MAX_ATTEMPTS", "999").has_value());
    OK(!attempts.freeze().has_value());
    ConfigBuilder gap(false);
    OK(gap.set("KARU_COALESCE_GAP", "not-a-number").has_value());
    OK(!gap.freeze().has_value());

    // Aliases fold onto one canonical name.
    EQS(canonical_option_name("aws_endpoint_url"), "AWS_S3_ENDPOINT");
    EQS(canonical_option_name("AWS_ENDPOINT_URL_S3"), "AWS_S3_ENDPOINT");
    EQS(canonical_option_name("AWS_DEFAULT_PROFILE"), "AWS_PROFILE");
    EQS(canonical_option_name("AWS_DEFAULT_REGION"), "AWS_REGION");
    EQS(canonical_option_name("KARU_MAX_RETRIES"), "KARU_MAX_ATTEMPTS");
    EQS(canonical_option_name("HUGGING_FACE_HUB_TOKEN"), "HF_TOKEN");
    EQS(canonical_option_name("CURL_CA_BUNDLE"), "KARU_HTTP_CA_BUNDLE");
    EQS(canonical_option_name("SSL_CERT_FILE"), "KARU_HTTP_CA_BUNDLE");
    EQS(canonical_option_name("SOURCE_PROXY_URL"), "SOURCE_ENDPOINT");
    EQS(canonical_option_name("aws_region"), "AWS_REGION");
}

void uri_rejections() {
    // Malformed byte windows and unknown handlers are refused with a reason.
    OK(!resolve("/vsisubfile/notanumber,/tmp/x").has_value());
    OK(!resolve("/vsisubfile/10_20").has_value());
    OK(!resolve("/vsisubfile/10_20,").has_value());
    OK(!resolve("/vsizip/archive.zip/member").has_value());
    OK(!resolve("/vsicurl/ftp://host/object").has_value());
    OK(!resolve("/vsis3/").has_value());
    OK(!resolve("/vsis3/bucket").has_value());
    OK(!resolve("ftp://host/object").has_value());
    OK(!resolve("").has_value());
    OK(!resolve("https://").has_value());
    OK(!resolve("file://").has_value());
    OK(!resolve("https://host/o#bytes=10").has_value());
    OK(!resolve("https://host/o#bytes=20-10").has_value());
    OK(!resolve("https://host/o#bytes=x-10").has_value());
    OK(!resolve("https://host/o#bytes=10-y").has_value());
    OK(!resolve("hf://datasets/org/repo@/file").has_value());

    // An unsupported /vsi handler is distinguishable from a malformed URI.
    auto unsupported = resolve("/vsizip/archive.zip/member");
    OK(!unsupported.has_value());
    OK(unsupported.error().code == ResolveErrorCode::Unsupported);
    auto malformed = resolve("ftp://host/object");
    OK(!malformed.has_value());
    OK(malformed.error().code != ResolveErrorCode::Unsupported);

    // Nesting is bounded rather than recursive without limit.
    std::string deep = "/tmp/object";
    for (int level = 0; level < 20; ++level)
        deep = "/vsisubfile/0_10," + deep;
    OK(!resolve(deep).has_value());
}

void endpoint_parsing() {
    using namespace karu::backends;
    OK(!split_url("host/path").has_value());
    OK(!split_url("ftp://host/path").has_value());
    OK(!split_url("https:///path").has_value());
    OK(!split_url("https://user@host/path").has_value());
    OK(!split_url("https://host/path#fragment").has_value());

    auto bare = split_url("https://host");
    OK(bare.has_value());
    if (bare) {
        EQS(bare->scheme, "https");
        EQS(bare->authority, "host");
        EQS(bare->path, "/");
    }
    auto queried = split_url("https://host/path?a=b");
    OK(queried.has_value());
    if (queried) {
        EQS(queried->path, "/path");
        EQS(queried->query, "a=b");
    }
    auto rootless = split_url("https://host?a=b");
    OK(rootless.has_value());
    if (rootless) {
        EQS(rootless->path, "/");
        EQS(rootless->query, "a=b");
    }
    auto uppercase = split_url("HTTPS://host/path");
    OK(uppercase.has_value());
    if (uppercase)
        EQS(uppercase->scheme, "https");

    // http_endpoint adds the scheme the caller asked for and rejects others.
    auto plain = http_endpoint("host:9000", "AWS_S3_ENDPOINT", false);
    OK(plain.has_value());
    if (plain)
        EQS(*plain, "http://host:9000");
    auto secure = http_endpoint("host", "AWS_S3_ENDPOINT", true);
    OK(secure.has_value());
    if (secure)
        EQS(*secure, "https://host");
    auto kept = http_endpoint("HTTP://host", "AWS_S3_ENDPOINT", true);
    OK(kept.has_value());
    if (kept)
        EQS(*kept, "HTTP://host");
    OK(!http_endpoint("ftp://host", "AWS_S3_ENDPOINT", true).has_value());

    EQS(without_trailing_slash("https://host///"), "https://host");
    EQS(without_trailing_slash("/"), "/");
    EQS(trim("  padded \t"), "padded");
    EQS(trim("   "), "");
    EQS(lower("MiXeD"), "mixed");
}

void transient_classification() {
    using karu::transport::detail::transient;
    // Transport failures that deserve another attempt.
    OK(transient(CURLE_OPERATION_TIMEDOUT, 0));
    OK(transient(CURLE_COULDNT_CONNECT, 0));
    OK(transient(CURLE_COULDNT_RESOLVE_HOST, 0));
    OK(transient(CURLE_GOT_NOTHING, 0));
    OK(transient(CURLE_SEND_ERROR, 0));
    OK(transient(CURLE_RECV_ERROR, 0));
    OK(transient(CURLE_PARTIAL_FILE, 0));
    OK(transient(CURLE_SSL_CONNECT_ERROR, 0));
    OK(transient(CURLE_HTTP2, 0));
    OK(transient(CURLE_HTTP2_STREAM, 0));
    // Throttling and server faults, whatever the transport said.
    OK(transient(CURLE_OK, 408));
    OK(transient(CURLE_OK, 425));
    OK(transient(CURLE_OK, 429));
    OK(transient(CURLE_OK, 500));
    OK(transient(CURLE_OK, 503));
    OK(transient(CURLE_OK, 599));
    // Everything the caller must not retry.
    OK(!transient(CURLE_OK, 200));
    OK(!transient(CURLE_OK, 206));
    OK(!transient(CURLE_OK, 400));
    OK(!transient(CURLE_OK, 403));
    OK(!transient(CURLE_OK, 404));
    OK(!transient(CURLE_OK, 412));
    OK(!transient(CURLE_OK, 416));
    OK(!transient(CURLE_OK, 600));
    OK(!transient(CURLE_UNSUPPORTED_PROTOCOL, 0));
    OK(!transient(CURLE_WRITE_ERROR, 206));

    using karu::transport::detail::status_for_http;
    OK(status_for_http(401) == KARU_ERR_AUTH);
    OK(status_for_http(403) == KARU_ERR_AUTH);
    OK(status_for_http(404) == KARU_ERR_NOT_FOUND);
    OK(status_for_http(412) == KARU_ERR_PRECONDITION);
    OK(status_for_http(416) == KARU_ERR_RANGE);
    OK(status_for_http(500) == KARU_ERR_HTTP);
    OK(status_for_http(302) == KARU_ERR_HTTP);
}

void redaction() {
    // Credentials ride in query strings and userinfo; neither may reach a log.
    EQS(redact_url("https://host/object?X-Amz-Signature=secret"), "https://host/object?<redacted>");
    EQS(redact_url("https://user:pass@host/object"), "https://<redacted>@host/object");
    EQS(redact_url("https://host/object#token"), "https://host/object#<redacted>");
    EQS(redact_url("https://host/object"), "https://host/object");
    EQS(redact_url("/local/path"), "/local/path");
    EQS(redact_url(""), "");
    // A '@' after the authority is part of the path, not userinfo.
    EQS(redact_url("https://host/path@name"), "https://host/path@name");
}

} // namespace

void test_status_and_paths() {
    SECTION("status table, VSI paths and configuration rejections");
    status_table();
    vsi_paths();
    prefix_matching();
    builder_rejections();
    uri_rejections();
    endpoint_parsing();
    transient_classification();
    redaction();
}

} // namespace karu::test
