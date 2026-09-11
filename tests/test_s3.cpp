#include "backends/aws_profile.hpp"
#include "backends/contract.hpp"
#include "backends/crypto.hpp"
#include "locator.hpp"
#include "process.hpp"
#include "test_support.hpp"

#include <chrono>

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
