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

    karu::ConfigBuilder public_builder(false);
    OK(public_builder.set("AWS_NO_SIGN_REQUEST", "YES"));
    OK(public_builder.set("AWS_S3_ENDPOINT", "http://127.0.0.1:9000"));
    karu::RequestBuilder public_request_builder(must_freeze(public_builder));
    auto public_request = public_request_builder.prepare(object, 0, 1);
    OK(public_request.has_value());
    if (public_request) {
        EQS(public_request->url, "http://127.0.0.1:9000/my-bucket/a%20b//c");
        OK(public_request->headers.empty());
    }
}

void test_managed_headers() {
    SECTION("managed cloud headers");
    ConfigBuilder builder(false);
    OK(builder.set("AWS_NO_SIGN_REQUEST", "YES"));
    OK(builder.set("GDAL_HTTP_HEADERS", "Range: bytes=5-6"));
    RequestBuilder request_builder(must_freeze(builder));
    OK(!request_builder.prepare(Locator{must_resolve("s3://bucket/key")}, 0, 1));
}

} // namespace karu::test
