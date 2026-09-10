#include "backends/contract.hpp"
#include "credential_cache.hpp"
#include "locator.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <latch>
#include <thread>

namespace karu::test {

void test_gcs_request() {
    SECTION("GCS request signing");
    karu::ConfigBuilder gcs_builder(false);
    OK(gcs_builder.set("GCS_HMAC_ACCESS_KEY_ID", "GOOG123"));
    OK(gcs_builder.set("GCS_HMAC_SECRET_ACCESS_KEY", "secret"));
    OK(gcs_builder.set("GCS_USER_PROJECT", "billing-project"));
    karu::RequestBuilder gcs_request_builder(must_freeze(gcs_builder));
    karu::Locator gcs{must_resolve("gs://bucket/a b")};
    auto gcs_request = gcs_request_builder.prepare(gcs, 3, 7);
    OK(gcs_request.has_value());
    if (gcs_request) {
        EQS(gcs_request->url, "https://storage.googleapis.com/bucket/a%20b");
        OK(header(*gcs_request, "Authorization").starts_with("GOOG1 GOOG123:"));
        EQS(header(*gcs_request, "x-goog-user-project"), "billing-project");
    }

    const ConfigSnapshot exact_config = must_freeze(gcs_builder);
    const Resolved exact_object = must_resolve("gs://bucket/a b");
    ProviderCredentials exact_credentials;
    exact_credentials.access_key_id = "GOOG123";
    exact_credentials.secret_access_key = "secret";
    const backends::RequestContext exact_context{
        exact_config, exact_object, &exact_credentials, "bytes=3-9", {}, {}, 1'440'938'160};
    auto exact_request = backends::gcs_provider().prepare_request(exact_context);
    OK(exact_request.has_value());
    if (exact_request) {
        EQS(header(*exact_request, "Date"), "Sun, 30 Aug 2015 12:36:00 GMT");
        EQS(header(*exact_request, "Authorization"), "GOOG1 GOOG123:ZIzQchNR2PmJ+kE3icykQrHrBaE=");
    }

    karu::ConfigBuilder invalid_gcs_endpoint(false);
    OK(invalid_gcs_endpoint.set("GCS_NO_SIGN_REQUEST", "YES"));
    OK(invalid_gcs_endpoint.set("GCS_ENDPOINT", "ftp://example.test"));
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
    std::int64_t expires_at = 0;
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
    out->expires_at = state.expires_at != 0
                          ? state.expires_at
                          : static_cast<std::int64_t>(std::time(nullptr)) + state.lifetime;
    return KARU_OK;
}

struct ConcurrentCallbackState {
    std::atomic<int> calls{0};
    std::atomic<int> active{0};
    std::atomic<int> maximum_active{0};
    const char* prefix = nullptr;
    std::atomic<std::int64_t> expires_at{0};
};

karu_status concurrent_credentials(void* data, karu_credentials_kind kind, const char*,
                                   karu_credentials* out) {
    auto& state = *static_cast<ConcurrentCallbackState*>(data);
    state.calls.fetch_add(1, std::memory_order_relaxed);
    const int active = state.active.fetch_add(1, std::memory_order_relaxed) + 1;
    int maximum = state.maximum_active.load(std::memory_order_relaxed);
    while (active > maximum && !state.maximum_active.compare_exchange_weak(
                                   maximum, active, std::memory_order_relaxed)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    state.active.fetch_sub(1, std::memory_order_relaxed);

    if (kind != KARU_CREDENTIALS_GCS)
        return KARU_ERR_CREDENTIALS;
    *out = karu_credentials{};
    out->bearer_token = "token";
    out->cache_prefix = state.prefix;
    const std::int64_t expires_at = state.expires_at.load(std::memory_order_relaxed);
    out->expires_at =
        expires_at != 0 ? expires_at : static_cast<std::int64_t>(std::time(nullptr)) + 3600;
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
        "GCS_NO_SIGN_REQUEST",
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

    ConcurrentCallbackState shared_state{.prefix = "/vsigs/bucket/"};
    karu::ConfigBuilder shared_builder(false);
    OK(shared_builder.set_provider(KARU_CREDENTIALS_GCS, concurrent_credentials, &shared_state,
                                   nullptr));
    karu::RequestBuilder shared_builder_request(must_freeze(shared_builder));
    std::latch shared_ready(4);
    std::latch shared_start(1);
    std::array<bool, 4> shared_results{};
    std::array<std::thread, 4> shared_workers;
    for (std::size_t index = 0; index < shared_workers.size(); ++index) {
        shared_workers[index] = std::thread([&, index] {
            shared_ready.count_down();
            shared_start.wait();
            shared_results[index] = shared_builder_request.prepare(object, index, 1).has_value();
        });
    }
    shared_ready.wait();
    shared_start.count_down();
    for (std::thread& worker : shared_workers)
        worker.join();
    for (const bool result : shared_results)
        OK(result);
    EQ(shared_state.calls.load(std::memory_order_relaxed), 1);

    ConcurrentCallbackState independent_state;
    karu::ConfigBuilder independent_builder(false);
    OK(independent_builder.set_provider(KARU_CREDENTIALS_GCS, concurrent_credentials,
                                        &independent_state, nullptr));
    karu::RequestBuilder independent_request(must_freeze(independent_builder));
    karu::Locator other{must_resolve("gs://bucket/other")};
    std::latch independent_ready(2);
    std::latch independent_start(1);
    std::array<bool, 2> independent_results{};
    std::array<std::thread, 2> independent_workers;
    independent_workers[0] = std::thread([&] {
        independent_ready.count_down();
        independent_start.wait();
        independent_results[0] = independent_request.prepare(object, 0, 1).has_value();
    });
    independent_workers[1] = std::thread([&] {
        independent_ready.count_down();
        independent_start.wait();
        independent_results[1] = independent_request.prepare(other, 0, 1).has_value();
    });
    independent_ready.wait();
    independent_start.count_down();
    for (std::thread& worker : independent_workers)
        worker.join();
    for (const bool result : independent_results)
        OK(result);
    EQ(independent_state.calls.load(std::memory_order_relaxed), 2);
    EQ(independent_state.maximum_active.load(std::memory_order_relaxed), 2);

    std::int64_t now = 1'000;
    CallbackState renewable{.expires_at = 1'100};
    ConfigBuilder renewable_builder(false);
    OK(renewable_builder.set_provider(KARU_CREDENTIALS_GCS, callback_credentials, &renewable,
                                      nullptr));
    const auto renewable_config = must_freeze(renewable_builder);
    CredentialCache renewable_cache([&] { return now; });
    OK(renewable_cache.custom(renewable_config, KARU_CREDENTIALS_GCS, "/vsigs/bucket/key"));
    now = 1'089;
    OK(renewable_cache.custom(renewable_config, KARU_CREDENTIALS_GCS, "/vsigs/bucket/key"));
    EQ(renewable.calls, 1);
    now = 1'090;
    renewable.expires_at = 1'200;
    OK(renewable_cache.custom(renewable_config, KARU_CREDENTIALS_GCS, "/vsigs/bucket/key"));
    EQ(renewable.calls, 2);

    CallbackState expired{.expires_at = 999};
    ConfigBuilder expired_builder(false);
    OK(expired_builder.set_provider(KARU_CREDENTIALS_GCS, callback_credentials, &expired, nullptr));
    CredentialCache expired_cache([&] { return now; });
    auto expired_result = expired_cache.custom(must_freeze(expired_builder), KARU_CREDENTIALS_GCS,
                                               "/vsigs/bucket/key");
    OK(!expired_result);
    if (!expired_result)
        EQ(expired_result.error().status, KARU_ERR_CREDENTIALS);

    now = 1'000;
    ConcurrentCallbackState concurrent_refresh;
    concurrent_refresh.prefix = "/vsigs/bucket/";
    concurrent_refresh.expires_at.store(1'100, std::memory_order_relaxed);
    ConfigBuilder concurrent_refresh_builder(false);
    OK(concurrent_refresh_builder.set_provider(KARU_CREDENTIALS_GCS, concurrent_credentials,
                                               &concurrent_refresh, nullptr));
    const auto concurrent_refresh_config = must_freeze(concurrent_refresh_builder);
    CredentialCache concurrent_refresh_cache([&] { return now; });
    OK(concurrent_refresh_cache.custom(concurrent_refresh_config, KARU_CREDENTIALS_GCS,
                                       "/vsigs/bucket/key"));
    now = 1'090;
    concurrent_refresh.expires_at.store(1'200, std::memory_order_relaxed);
    std::latch refresh_ready(4);
    std::latch refresh_start(1);
    std::array<bool, 4> refresh_results{};
    std::array<std::thread, 4> refresh_workers;
    for (std::size_t index = 0; index < refresh_workers.size(); ++index) {
        refresh_workers[index] = std::thread([&, index] {
            refresh_ready.count_down();
            refresh_start.wait();
            refresh_results[index] =
                concurrent_refresh_cache
                    .custom(concurrent_refresh_config, KARU_CREDENTIALS_GCS, "/vsigs/bucket/key")
                    .has_value();
        });
    }
    refresh_ready.wait();
    refresh_start.count_down();
    for (std::thread& worker : refresh_workers)
        worker.join();
    for (const bool result : refresh_results)
        OK(result);
    EQ(concurrent_refresh.calls.load(std::memory_order_relaxed), 2);

    CallbackState boundary{.prefix = "/vsigs/bucket/private"};
    ConfigBuilder boundary_builder(false);
    OK(boundary_builder.set_provider(KARU_CREDENTIALS_GCS, callback_credentials, &boundary,
                                     nullptr));
    CredentialCache boundary_cache;
    OK(!boundary_cache.custom(must_freeze(boundary_builder), KARU_CREDENTIALS_GCS,
                              "/vsigs/bucket/private-copy/key"));
}

} // namespace karu::test
