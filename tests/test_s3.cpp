#include "locator.hpp"
#include "test_support.hpp"

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
    OK(header(*request, "Authorization").starts_with("AWS4-HMAC-SHA256 "));
    OK(header(*request, "Authorization").find("range;") != std::string::npos);
    OK(header(*request, "Authorization").find("if-match;") != std::string::npos);
    EQS(header(*request, "if-match"), "\"etag\"");
    EQS(header(*request, "x-amz-content-sha256"), "UNSIGNED-PAYLOAD");
    OK(!request->http.follow_redirects);

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
    }

    karu::ConfigBuilder inaccessible(false);
    const std::string oversized_path(5000, 'x');
    OK(inaccessible.set("AWS_CONFIG_FILE", oversized_path.c_str()));
    karu::RequestBuilder inaccessible_builder(must_freeze(inaccessible));
    auto inaccessible_request = inaccessible_builder.prepare(object, 0, 1);
    OK(!inaccessible_request);
    if (!inaccessible_request)
        EQ(inaccessible_request.error().status, KARU_ERR_CREDENTIALS);
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
