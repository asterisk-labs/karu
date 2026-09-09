#include "locator.hpp"
#include "test_support.hpp"

#include <ctime>

namespace karu::test {

void test_gcs_request() {
    SECTION("GCS request signing");
    karu::ConfigBuilder gcs_builder(false);
    OK(gcs_builder.set("GS_ACCESS_KEY_ID", "GOOG123"));
    OK(gcs_builder.set("GS_SECRET_ACCESS_KEY", "secret"));
    OK(gcs_builder.set("GS_USER_PROJECT", "billing-project"));
    karu::RequestBuilder gcs_request_builder(must_freeze(gcs_builder));
    karu::Locator gcs{must_resolve("gs://bucket/a b")};
    auto gcs_request = gcs_request_builder.prepare(gcs, 3, 7);
    OK(gcs_request.has_value());
    if (gcs_request) {
        EQS(gcs_request->url, "https://storage.googleapis.com/bucket/a%20b");
        OK(header(*gcs_request, "Authorization").starts_with("GOOG1 GOOG123:"));
        EQS(header(*gcs_request, "x-goog-user-project"), "billing-project");
    }

    karu::ConfigBuilder invalid_gcs_endpoint(false);
    OK(invalid_gcs_endpoint.set("GS_NO_SIGN_REQUEST", "YES"));
    OK(invalid_gcs_endpoint.set("CPL_GS_ENDPOINT", "ftp://example.test"));
    karu::RequestBuilder invalid_gcs(must_freeze(invalid_gcs_endpoint));
    auto bad_endpoint = invalid_gcs.prepare(gcs, 0, 1);
    OK(!bad_endpoint);
    if (!bad_endpoint)
        EQ(bad_endpoint.error().status, KARU_ERR_CONFIG);
}

struct CallbackState {
    int calls = 0;
    const char* prefix = "/vsigs/bucket/";
};

karu_status callback_credentials(void* data, karu_credentials_kind kind, const char*,
                                 karu_credentials* out) {
    auto& state = *static_cast<CallbackState*>(data);
    ++state.calls;
    if (kind != KARU_CREDENTIALS_GCS)
        return KARU_ERR_CREDENTIALS;
    *out = karu_credentials{};
    out->bearer_token = "token";
    out->cache_prefix = state.prefix;
    out->expires_at = static_cast<std::int64_t>(std::time(nullptr)) + 3600;
    return KARU_OK;
}

void test_renewable_callback_cache() {
    SECTION("credential provider");
    CallbackState state;
    karu::ConfigBuilder builder(false);
    OK(builder.set_provider(KARU_CREDENTIALS_GCS, callback_credentials, &state, nullptr));
    karu::RequestBuilder request_builder(must_freeze(builder));
    karu::Locator object{must_resolve("gs://bucket/key")};
    auto first = request_builder.prepare(object, 0, 1);
    auto second = request_builder.prepare(object, 100, 1);
    OK(first.has_value());
    OK(second.has_value());
    EQ(state.calls, 1);
    if (second)
        EQS(header(*second, "Authorization"), "Bearer token");

    CallbackState uncached_state{.prefix = nullptr};
    karu::ConfigBuilder uncached_builder(false);
    OK(uncached_builder.set_provider(KARU_CREDENTIALS_GCS, callback_credentials, &uncached_state,
                                     nullptr));
    karu::RequestBuilder uncached(must_freeze(uncached_builder));
    OK(uncached.prepare(object, 0, 1));
    OK(uncached.prepare(object, 1, 1));
    EQ(uncached_state.calls, 2);

    CallbackState invalid_state{.prefix = "/vsigs/other/"};
    karu::ConfigBuilder invalid_builder(false);
    OK(invalid_builder.set_provider(KARU_CREDENTIALS_GCS, callback_credentials, &invalid_state,
                                    nullptr));
    karu::RequestBuilder invalid(must_freeze(invalid_builder));
    OK(!invalid.prepare(object, 0, 1));
}

} // namespace karu::test
