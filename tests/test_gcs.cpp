#include "backends/contract.hpp"
#include "credential_cache.hpp"
#include "locator.hpp"
#include "test_support.hpp"

#include <array>
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
    int lifetime = 3600;
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
    out->expires_at = static_cast<std::int64_t>(std::time(nullptr)) + state.lifetime;
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

    CallbackState short_lived{.lifetime = 30};
    karu::ConfigBuilder short_builder(false);
    OK(short_builder.set_provider(KARU_CREDENTIALS_GCS, callback_credentials, &short_lived,
                                  nullptr));
    karu::RequestBuilder short_cache(must_freeze(short_builder));
    OK(short_cache.prepare(object, 0, 1));
    OK(short_cache.prepare(object, 1, 1));
    EQ(short_lived.calls, 1);

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

    static int native_calls = 0;
    constexpr std::array<std::string_view, 0> no_options{};
    const karu::backends::CloudProvider provider{
        karu::Backend::Gcs,
        KARU_CREDENTIALS_GCS,
        "GS_NO_SIGN_REQUEST",
        false,
        no_options,
        [](const karu::ConfigSnapshot&, std::string_view) {
            ++native_calls;
            karu::ProviderCredentials credentials;
            credentials.bearer_token = "static";
            return std::expected<karu::ProviderCredentials, karu::RequestError>{
                std::move(credentials)};
        },
        [](const karu::backends::RequestContext&) {
            return std::expected<karu::PreparedRequest, karu::RequestError>{
                karu::PreparedRequest{"https://example.test", {}}};
        }};
    native_calls = 0;
    karu::CredentialCache native_cache;
    const auto snapshot = must_freeze(karu::ConfigBuilder(false));
    OK(native_cache.native(snapshot, provider, "/vsigs/bucket/key"));
    OK(native_cache.native(snapshot, provider, "/vsigs/bucket/key"));
    EQ(native_calls, 1);
    native_cache.invalidate(snapshot, provider, "/vsigs/bucket/key", false);
    OK(native_cache.native(snapshot, provider, "/vsigs/bucket/key"));
    EQ(native_calls, 2);
}

} // namespace karu::test
