#include "locator.hpp"
#include "platform.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace karu::test {
namespace {

struct SourceCallbackState {
    int calls = 0;
    std::string path;
};

karu_status source_credentials(void* user_data, karu_credentials_kind kind, const char* path,
                               karu_credentials* out) {
    auto& state = *static_cast<SourceCallbackState*>(user_data);
    ++state.calls;
    state.path = path;
    if (kind != KARU_CREDENTIALS_SOURCE)
        return KARU_ERR_CREDENTIALS;
    out->access_key_id = "SOURCEKEY";
    out->secret_access_key = "source-secret";
    out->session_token = "source-session";
    return KARU_OK;
}

} // namespace

void test_source_request() {
    SECTION("Source Cooperative requests");
    const Locator object{must_resolve("source://account/product/a b//data.cozip")};

    RequestBuilder anonymous(must_freeze(ConfigBuilder(false)));
    auto public_request = anonymous.prepare(object, 10, 20, {}, "\"etag\"");
    OK(public_request.has_value());
    if (public_request) {
        EQS(public_request->url, "https://data.source.coop/account/product/a%20b//data.cozip");
        EQS(header(*public_request, "If-Match"), "\"etag\"");
        OK(header(*public_request, "Authorization").empty());
    }

    // Ambient AWS credentials belong to /vsis3/ and must never be forwarded
    // to the Source proxy.
    ConfigBuilder aws(false);
    OK(aws.set("AWS_ACCESS_KEY_ID", "AWSKEY"));
    OK(aws.set("AWS_SECRET_ACCESS_KEY", "aws-secret"));
    RequestBuilder aws_only(must_freeze(aws));
    auto isolated = aws_only.prepare(object, 0, 1);
    OK(isolated.has_value());
    if (isolated)
        OK(header(*isolated, "Authorization").empty());

    ConfigBuilder direct(false);
    OK(direct.set("SOURCE_ACCESS_KEY_ID", "SOURCEKEY"));
    OK(direct.set("SOURCE_SECRET_ACCESS_KEY", "source-secret"));
    OK(direct.set("SOURCE_SESSION_TOKEN", "source-session"));
    RequestBuilder signed_builder(must_freeze(direct));
    auto signed_request = signed_builder.prepare(object, 10, 20);
    OK(signed_request.has_value());
    if (signed_request) {
        OK(header(*signed_request, "Authorization").starts_with("AWS4-HMAC-SHA256 "));
        EQS(header(*signed_request, "x-amz-security-token"), "source-session");
    }

    const auto profile_path = std::filesystem::temp_directory_path() /
                              ("karu_source_profile_" + std::to_string(os::pid()) + ".ini");
    {
        std::ofstream profile(profile_path, std::ios::trunc);
        profile << "[profile private]\n"
                   "aws_access_key_id = PROFILEKEY\n"
                   "aws_secret_access_key = profile-secret\n"
                   "aws_session_token = profile-session\n"
                   "region = eu-west-1\n";
        OK(profile.good());
    }
    ConfigBuilder profile(false);
    OK(profile.set("SOURCE_PROFILE", "private"));
    OK(profile.set("SOURCE_CONFIG_FILE", profile_path.string().c_str()));
    RequestBuilder profile_builder(must_freeze(profile));
    auto profile_request = profile_builder.prepare(object, 0, 1);
    OK(profile_request.has_value());
    if (profile_request) {
        OK(header(*profile_request, "Authorization").find("Credential=PROFILEKEY/") !=
           std::string::npos);
        OK(header(*profile_request, "Authorization").find("/eu-west-1/s3/aws4_request") !=
           std::string::npos);
    }
    std::filesystem::remove(profile_path);

    ConfigBuilder custom(false);
    SourceCallbackState state;
    OK(custom.set_provider(KARU_CREDENTIALS_SOURCE, source_credentials, &state, nullptr));
    OK(custom.set("SOURCE_ENDPOINT", "https://proxy.example.test/base"));
    RequestBuilder custom_builder(must_freeze(custom));
    auto custom_request = custom_builder.prepare(object, 0, 1);
    OK(custom_request.has_value());
    EQ(state.calls, 1);
    EQS(state.path, "/vsisource/account/product/a b//data.cozip");
    if (custom_request) {
        EQS(custom_request->url,
            "https://proxy.example.test/base/account/product/a%20b//data.cozip");
        OK(!header(*custom_request, "Authorization").empty());
    }

    ConfigBuilder forced_public(false);
    OK(forced_public.set("SOURCE_ACCESS_KEY_ID", "unused"));
    OK(forced_public.set("SOURCE_SECRET_ACCESS_KEY", "unused"));
    OK(forced_public.set("SOURCE_NO_SIGN_REQUEST", "YES"));
    RequestBuilder forced_public_builder(must_freeze(forced_public));
    auto unsigned_request = forced_public_builder.prepare(object, 0, 1);
    OK(unsigned_request.has_value());
    if (unsigned_request)
        OK(header(*unsigned_request, "Authorization").empty());

    ConfigBuilder missing(false);
    OK(missing.set("SOURCE_NO_SIGN_REQUEST", "NO"));
    RequestBuilder missing_builder(must_freeze(missing));
    auto missing_request = missing_builder.prepare(object, 0, 1);
    OK(!missing_request);
    if (!missing_request)
        EQ(missing_request.error().status, KARU_ERR_CREDENTIALS);

    if (std::getenv("KARU_TEST_SOURCE_LIVE") == nullptr)
        return;

    SECTION("Source Cooperative live read");
    karu_config* live_config = nullptr;
    karu_client* live_client = nullptr;
    karu_locator* live_object = nullptr;
    karu_locator* live_window = nullptr;
    EQ(karu_config_create_empty(&live_config), KARU_OK);
    EQ(karu_client_create(live_config, &live_client), KARU_OK);
    EQ(karu_resolve("source://source/metadata-catalog/catalog.jsonl", &live_object), KARU_OK);
    std::uint64_t live_size = 0;
    EQ(karu_client_size(live_client, live_object, &live_size), KARU_OK);
    OK(live_size > 64);
    std::array<unsigned char, 64> bytes{};
    const karu_req read{live_object, 0, bytes.size(), bytes.data(), nullptr, nullptr};
    EQ(karu_client_fetch(live_client, &read, 1), KARU_OK);
    EQ(bytes.front(), static_cast<unsigned char>('{'));
    EQ(karu_resolve("/vsisubfile/1_10,/vsisource/source/metadata-catalog/catalog.jsonl",
                    &live_window),
       KARU_OK);
    std::array<unsigned char, 10> window_bytes{};
    const karu_req window_read{live_window,         0,       window_bytes.size(),
                               window_bytes.data(), nullptr, nullptr};
    EQ(karu_client_fetch(live_client, &window_read, 1), KARU_OK);
    OK(std::equal(window_bytes.begin(), window_bytes.end(), bytes.begin() + 1));
    karu_locator_free(live_window);
    karu_locator_free(live_object);
    karu_client_free(live_client);
    karu_config_free(live_config);
}

} // namespace karu::test
