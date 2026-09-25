#include "backends/aws_profile.hpp"
#include "backends/contract.hpp"
#include "backends/crypto.hpp"
#include "locator.hpp"
#include "process.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace karu::test {

void test_s3_request() {
    SECTION("S3 request signing");
    karu::ConfigBuilder builder(false);
    OK(builder.set("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE"));
    OK(builder.set("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"));
    OK(builder.set("AWS_REGION", "us-west-2"));
    karu::RequestBuilder request_builder(must_freeze(builder));
    karu::Locator object{must_resolve("/vsis3/my-bucket/a b//c")};
    auto request = request_builder.prepare(object, 10, 20, {}, "\"etag\"");
    OK(request.has_value());
    if (!request)
        return;
    EQS(request->url, "https://my-bucket.s3.us-west-2.amazonaws.com/a%20b//c");
    EQS(request->range, "bytes=10-29");
    OK(header(*request, "Authorization").starts_with("AWS4-HMAC-SHA256 "));
    OK(header(*request, "Authorization").find("range;") != std::string::npos);
    OK(header(*request, "Authorization").find("if-match;") != std::string::npos);
    EQS(header(*request, "if-match"), "\"etag\"");
    EQS(header(*request, "x-amz-content-sha256"), "UNSIGNED-PAYLOAD");
    OK(!request->http.follow_redirects);

    const ConfigSnapshot exact_config = must_freeze(ConfigBuilder(false));
    const Resolved exact_object = must_resolve("s3://examplebucket/test file");
    ProviderCredentials exact_credentials;
    exact_credentials.access_key_id = "AKIDEXAMPLE";
    exact_credentials.secret_access_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    exact_credentials.session_token = "session";
    const backends::RequestContext exact_context{exact_config,  exact_object, &exact_credentials,
                                                 "bytes=10-29", {},           "\"etag\"",
                                                 1'440'938'160};
    auto exact_request = backends::s3_provider().prepare_request(exact_context);
    OK(exact_request.has_value());
    if (exact_request) {
        EQS(header(*exact_request, "x-amz-date"), "20150830T123600Z");
        EQS(header(*exact_request, "Authorization"),
            "AWS4-HMAC-SHA256 "
            "Credential=AKIDEXAMPLE/20150830/us-east-1/s3/aws4_request, "
            "SignedHeaders=host;if-match;range;x-amz-content-sha256;x-amz-date;"
            "x-amz-security-token, "
            "Signature=a47ce3c9bbb62233c87e37d9e4265301fa0a5d5d0ab60ad9ab9d8baebd8165bc");
    }

    // Each cache-key field must invalidate the derived key. Switching back
    // must reproduce the original signature.
    const auto exact_signature = [&](const backends::RequestContext& context) {
        auto prepared = backends::s3_provider().prepare_request(context);
        return prepared ? header(*prepared, "Authorization") : std::string();
    };
    ProviderCredentials rotated_credentials = exact_credentials;
    rotated_credentials.secret_access_key = "a-different-secret";
    const backends::RequestContext rotated_context{
        exact_config, exact_object, &rotated_credentials, "bytes=10-29",
        {},           "\"etag\"",   1'440'938'160};
    const backends::RequestContext next_day_context{
        exact_config, exact_object, &exact_credentials,    "bytes=10-29",
        {},           "\"etag\"",   1'440'938'160 + 86'400};
    const backends::RequestContext other_region_context{
        exact_config, exact_object, &exact_credentials, "bytes=10-29",
        "eu-west-1",  "\"etag\"",   1'440'938'160};
    const std::string reference = exact_request ? header(*exact_request, "Authorization") : "";
    const std::string rotated = exact_signature(rotated_context);
    const std::string next_day = exact_signature(next_day_context);
    const std::string other_region = exact_signature(other_region_context);
    OK(!rotated.empty() && rotated != reference);
    OK(!next_day.empty() && next_day != reference &&
       next_day.find("/20150831/") != std::string::npos);
    OK(!other_region.empty() && other_region != reference &&
       other_region.find("/eu-west-1/") != std::string::npos);
    EQS(exact_signature(exact_context), reference);
    EQS(exact_signature(rotated_context), rotated);

    karu::Locator dotted{must_resolve("s3://bucket.with.dots/key")};
    auto dotted_request = request_builder.prepare(dotted, 0, 1);
    OK(dotted_request.has_value());
    if (dotted_request)
        EQS(dotted_request->url, "https://s3.us-west-2.amazonaws.com/bucket.with.dots/key");

    karu::ConfigBuilder explicit_virtual(false);
    OK(explicit_virtual.set("AWS_ACCESS_KEY_ID", "access"));
    OK(explicit_virtual.set("AWS_SECRET_ACCESS_KEY", "secret"));
    OK(explicit_virtual.set("AWS_VIRTUAL_HOSTING", "YES"));
    karu::RequestBuilder explicit_virtual_builder(must_freeze(explicit_virtual));
    auto explicit_dotted = explicit_virtual_builder.prepare(dotted, 0, 1);
    OK(explicit_dotted.has_value());
    if (explicit_dotted)
        EQS(explicit_dotted->url, "https://bucket.with.dots.s3.us-east-1.amazonaws.com/key");

    karu::ConfigBuilder default_port(false);
    OK(default_port.set("AWS_ACCESS_KEY_ID", "access"));
    OK(default_port.set("AWS_SECRET_ACCESS_KEY", "secret"));
    OK(default_port.set("AWS_S3_ENDPOINT", "https://storage.example.test:443"));
    karu::RequestBuilder default_port_builder(must_freeze(default_port));
    karu::ConfigBuilder implicit_port(false);
    OK(implicit_port.set("AWS_ACCESS_KEY_ID", "access"));
    OK(implicit_port.set("AWS_SECRET_ACCESS_KEY", "secret"));
    OK(implicit_port.set("AWS_S3_ENDPOINT", "https://storage.example.test"));
    karu::RequestBuilder implicit_port_builder(must_freeze(implicit_port));
    const karu::Locator port_object{must_resolve("s3://bucket/key")};
    auto explicit_port_request = default_port_builder.prepare(port_object, 0, 1);
    auto implicit_port_request = implicit_port_builder.prepare(port_object, 0, 1);
    OK(explicit_port_request.has_value());
    OK(implicit_port_request.has_value());
    if (explicit_port_request && implicit_port_request &&
        header(*explicit_port_request, "x-amz-date") ==
            header(*implicit_port_request, "x-amz-date")) {
        EQS(header(*explicit_port_request, "Authorization"),
            header(*implicit_port_request, "Authorization"));
    }

    karu::ConfigBuilder public_builder(false);
    OK(public_builder.set("AWS_NO_SIGN_REQUEST", "YES"));
    OK(public_builder.set("AWS_S3_ENDPOINT", "http://127.0.0.1:9000"));
    karu::RequestBuilder public_request_builder(must_freeze(public_builder));
    auto public_request = public_request_builder.prepare(object, 0, 1);
    OK(public_request.has_value());
    if (public_request) {
        EQS(public_request->url, "http://127.0.0.1:9000/my-bucket/a%20b//c");
        OK(public_request->headers.empty());
        OK(public_request->http.follow_redirects);
        OK(!public_request->http.same_origin_redirects_only);
    }

    karu::ConfigBuilder inaccessible(false);
    const std::string oversized_path(5000, 'x');
    OK(inaccessible.set("AWS_CONFIG_FILE", oversized_path.c_str()));
    karu::RequestBuilder inaccessible_builder(must_freeze(inaccessible));
    auto inaccessible_request = inaccessible_builder.prepare(object, 0, 1);
    OK(!inaccessible_request);
    if (!inaccessible_request)
        EQ(inaccessible_request.error().status, KARU_ERR_CREDENTIALS);

    const auto unavailable_hmac = karu::backends::hmac(nullptr, {}, "value");
    OK(!unavailable_hmac);
    if (!unavailable_hmac)
        EQ(unavailable_hmac.error().status, KARU_ERR_CREDENTIALS);

    SECTION("SHA-256 and HMAC-SHA256");
    const auto hex = [](std::span<const unsigned char> bytes) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        for (const unsigned char byte : bytes) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
        return result;
    };
    const auto as_bytes = [](std::string_view text) {
        return std::span(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    };
    const auto empty_digest = karu::backends::sha256("");
    OK(empty_digest.has_value());
    if (empty_digest)
        EQS(hex(*empty_digest), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const auto abc_digest = karu::backends::sha256("abc");
    OK(abc_digest.has_value());
    if (abc_digest)
        EQS(hex(*abc_digest), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // RFC 4231 cases 1, 2, 6 and 7 include keys longer than SHA-256's block size.
    const std::string short_key(20, '\x0b');
    const std::string long_key(131, '\xaa');
    struct HmacCase {
        std::string_view key;
        std::string_view data;
        std::string_view expected;
    };
    const std::array hmac_cases{
        HmacCase{short_key, "Hi There",
                 "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        HmacCase{"Jefe", "what do ya want for nothing?",
                 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        HmacCase{long_key, "Test Using Larger Than Block-Size Key - Hash Key First",
                 "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
        HmacCase{long_key,
                 "This is a test using a larger than block-size key and a larger than "
                 "block-size data. The key needs to be hashed before being used by the HMAC "
                 "algorithm.",
                 "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"},
    };
    for (const HmacCase& entry : hmac_cases) {
        const auto mac = karu::backends::hmac_sha256(as_bytes(entry.key), entry.data);
        OK(mac.has_value());
        if (mac)
            EQS(hex(*mac), entry.expected);
    }
    // Keys around the 64-byte block boundary, checked against OpenSSL's HMAC.
    for (const std::size_t size : {0u, 1u, 63u, 64u, 65u, 200u}) {
        const std::string key(size, 'k');
        const auto fast = karu::backends::hmac_sha256(as_bytes(key), "boundary");
        const auto openssl = karu::backends::hmac(EVP_sha256(), as_bytes(key), "boundary");
        OK(fast.has_value() && openssl.has_value());
        if (fast && openssl)
            OK(std::ranges::equal(*fast, *openssl));
    }

    SECTION("AWS credential process");
    karu::backends::AwsProfile slow_profile;
#ifdef _WIN32
    slow_profile.values.emplace("credential_process", "ping 127.0.0.1 -n 6 >NUL");
#else
    slow_profile.values.emplace("credential_process", "sleep 5");
#endif
    const auto started = std::chrono::steady_clock::now();
    auto timed_out = karu::backends::credentials_from_aws_profile(slow_profile, "AWS", 1);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    OK(!timed_out);
    if (!timed_out)
        EQ(timed_out.error().status, KARU_TIMEOUT);
    OK(elapsed < std::chrono::seconds(4));

#ifdef _WIN32
    constexpr std::string_view ready_command = "echo ready";
    constexpr std::string_view noisy_command =
        "for /L %i in (1,1,10000) do @echo 12345678901234567890";
    constexpr std::string_view failing_command = "exit /B 7";
#else
    constexpr std::string_view ready_command = "printf ready";
    constexpr std::string_view noisy_command = "while :; do printf 12345678901234567890; done";
    constexpr std::string_view failing_command = "exit 7";
#endif
    auto ready = os::run_command(ready_command, 1'024, std::chrono::seconds(2));
    OK(ready.has_value());
    if (ready) {
        EQ(ready->exit_code, 0);
        OK(ready->output.find("ready") != std::string::npos);
    }
    auto limited = os::run_command(noisy_command, 1'024, std::chrono::seconds(2));
    OK(limited.has_value());
    if (limited)
        OK(limited->output_limit_exceeded);
    auto failed = os::run_command(failing_command, 1'024, std::chrono::seconds(2));
    OK(failed.has_value());
    if (failed)
        EQ(failed->exit_code, 7);
}

void test_managed_headers() {
    SECTION("managed cloud headers");
    ConfigBuilder builder(false);
    OK(builder.set("AWS_NO_SIGN_REQUEST", "YES"));
    OK(builder.set("KARU_HTTP_HEADERS", "Range: bytes=5-6"));
    RequestBuilder request_builder(must_freeze(builder));
    OK(!request_builder.prepare(Locator{must_resolve("s3://bucket/key")}, 0, 1));
}

} // namespace karu::test
