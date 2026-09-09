// End-to-end HTTP checks against tests/http_server.py.

#include "karu/karu.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <span>
#include <string>
#include <thread>

namespace {

int failures = 0;

unsigned char expected_byte(std::size_t offset) {
    return static_cast<unsigned char>((offset * 31 + 7) & 0xff);
}

void check(bool condition, const char* message) {
    if (condition)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL: %s (%s)\n", message, karu_last_error());
}

karu_status fetch(karu_client* client, const std::string& uri, std::uint64_t offset,
                  std::span<unsigned char> destination, const char* if_match = nullptr) {
    karu_locator* locator = nullptr;
    const karu_status resolved = karu_resolve(uri.c_str(), &locator);
    if (resolved != KARU_OK)
        return resolved;
    const karu_req request{locator, offset,  destination.size(), destination.data(),
                           nullptr, if_match};
    const karu_status status = karu_client_fetch(client, &request, 1);
    karu_locator_free(locator);
    return status;
}

void check_bytes(std::span<const unsigned char> bytes, std::size_t offset, const char* message) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] != expected_byte(offset + index)) {
            check(false, message);
            return;
        }
    }
}

struct RefreshState {
    int calls = 0;
};

karu_status refresh_credentials(void* data, karu_credentials_kind kind, const char*,
                                karu_credentials* out) {
    auto& state = *static_cast<RefreshState*>(data);
    ++state.calls;
    if (kind != KARU_CREDENTIALS_GCS)
        return KARU_ERR_CREDENTIALS;
    *out = karu_credentials{};
    out->bearer_token = state.calls == 1 ? "stale" : "fresh";
    out->cache_prefix = "/vsigs/bucket/";
    out->expires_at = static_cast<std::int64_t>(std::time(nullptr)) + 3600;
    return KARU_OK;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::string base = "http://127.0.0.1:" + std::string(argv[1]);

    karu_config* config = nullptr;
    karu_client* client = nullptr;
    check(karu_config_create_empty(&config) == KARU_OK, "create config");
    check(karu_config_set_option(config, "KARU_MAX_ATTEMPTS", "2") == KARU_OK, "set attempts");
    check(karu_config_set_option(config, "KARU_COALESCE_GAP", "1") == KARU_OK, "set coalescing");
    check(karu_client_create(config, &client) == KARU_OK, "create client");
    karu_config_free(config);

    std::array<unsigned char, 333> range{};
    check(fetch(client, base + "/object", 127, range) == KARU_OK, "206 range read");
    check_bytes(range, 127, "206 range contents");

    karu_locator* object = nullptr;
    check(karu_resolve((base + "/object").c_str(), &object) == KARU_OK, "resolve object");
    std::uint64_t size = 0;
    check(karu_client_size(client, object, &size) == KARU_OK && size == 4096,
          "size from Content-Range");

    karu_locator* reusable_size = nullptr;
    check(karu_resolve((base + "/size-reuse").c_str(), &reusable_size) == KARU_OK,
          "resolve reusable size endpoint");
    for (int attempt = 0; attempt < 10; ++attempt) {
        check(karu_client_size(client, reusable_size, &size) == KARU_OK && size == 4096,
              "successive size calls reuse their connection");
    }
    karu_locator_free(reusable_size);

    karu_locator* concurrent_size = nullptr;
    check(karu_resolve((base + "/concurrent-size").c_str(), &concurrent_size) == KARU_OK,
          "resolve concurrent size endpoint");
    std::atomic<int> size_failures{0};
    std::array<std::thread, 4> size_workers;
    for (std::thread& worker : size_workers) {
        worker = std::thread([&] {
            for (int attempt = 0; attempt < 4; ++attempt) {
                std::uint64_t concurrent_result = 0;
                if (karu_client_size(client, concurrent_size, &concurrent_result) != KARU_OK ||
                    concurrent_result != 4096) {
                    size_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& worker : size_workers)
        worker.join();
    check(size_failures.load(std::memory_order_relaxed) == 0, "concurrent size calls");
    karu_locator_free(concurrent_size);

    karu_locator* unknown_size = nullptr;
    check(karu_resolve((base + "/unknown-size").c_str(), &unknown_size) == KARU_OK,
          "resolve unknown size");
    check(karu_client_size(client, unknown_size, &size) == KARU_ERR_HTTP,
          "unknown Content-Range total rejected");
    karu_locator_free(unknown_size);

    karu_locator* oversized_size_body = nullptr;
    check(karu_resolve((base + "/oversized-size-body").c_str(), &oversized_size_body) == KARU_OK,
          "resolve oversized size body endpoint");
    check(karu_client_size(client, oversized_size_body, &size) == KARU_ERR_HTTP,
          "oversized size response rejected");
    karu_locator_free(oversized_size_body);

    karu_locator* missing_size_body = nullptr;
    check(karu_resolve((base + "/missing-size-body").c_str(), &missing_size_body) == KARU_OK,
          "resolve missing size body endpoint");
    check(karu_client_size(client, missing_size_body, &size) == KARU_ERR_HTTP,
          "missing size response body rejected");
    karu_locator_free(missing_size_body);

    karu_locator* invalid_empty_size = nullptr;
    check(karu_resolve((base + "/invalid-empty-size").c_str(), &invalid_empty_size) == KARU_OK,
          "resolve invalid empty size");
    check(karu_client_size(client, invalid_empty_size, &size) == KARU_ERR_HTTP,
          "malformed 416 Content-Range rejected");
    karu_locator_free(invalid_empty_size);

    karu_locator* empty = nullptr;
    check(karu_resolve((base + "/empty").c_str(), &empty) == KARU_OK, "resolve empty object");
    check(karu_client_size(client, empty, &size) == KARU_OK && size == 0,
          "empty object size from Content-Range");
    karu_locator_free(empty);

    karu_locator* no_content = nullptr;
    check(karu_resolve((base + "/no-content").c_str(), &no_content) == KARU_OK,
          "resolve no-content response");
    check(karu_client_size(client, no_content, &size) == KARU_ERR_HTTP,
          "unexpected size response rejected");
    karu_locator_free(no_content);

    std::array<unsigned char, 100> ignored{};
    check(fetch(client, base + "/ignore-range", 100, ignored) == KARU_OK, "server ignoring Range");
    check_bytes(ignored, 100, "ignored Range contents");

    std::array<unsigned char, 16> clipped{};
    check(fetch(client, base + "/object", 4090, clipped) == KARU_ERR_RANGE,
          "range clipped at object end");
    check_bytes(std::span(clipped).first(6), 4090, "clipped range contents");

    std::array<unsigned char, 16> small{};
    check(fetch(client, base + "/redirect", 21, small) == KARU_OK, "same-origin redirect");
    check_bytes(small, 21, "redirect contents");
    check(fetch(client, base + "/redirect-ftp", 0, small) == KARU_ERR_NETWORK,
          "non-HTTP redirect rejected");
    karu_locator* redirected = nullptr;
    check(karu_resolve((base + "/redirect-ftp").c_str(), &redirected) == KARU_OK,
          "resolve non-HTTP redirect");
    check(karu_client_size(client, redirected, &size) == KARU_ERR_NETWORK,
          "non-HTTP size redirect rejected");
    karu_locator_free(redirected);
    check(fetch(client, base + "/retry", 31, small) == KARU_OK, "retry after 503");
    check_bytes(small, 31, "retry contents");
    check(fetch(client, base + "/large-error-retry", 31, small) == KARU_OK,
          "large error body keeps the connection reusable");
    check_bytes(small, 31, "large error retry contents");

    karu_locator* large_size = nullptr;
    check(karu_resolve((base + "/large-size-retry").c_str(), &large_size) == KARU_OK,
          "resolve large-error size endpoint");
    check(karu_client_size(client, large_size, &size) == KARU_OK && size == 4096,
          "large size error keeps the connection reusable");
    karu_locator_free(large_size);

    karu_config* timeout_config = nullptr;
    karu_client* timeout_client = nullptr;
    check(karu_config_create_empty(&timeout_config) == KARU_OK, "create timeout config");
    check(karu_config_set_option(timeout_config, "KARU_MAX_ATTEMPTS", "16") == KARU_OK,
          "set timeout attempts");
    check(karu_config_set_option(timeout_config, "KARU_REQUEST_TIMEOUT", "1") == KARU_OK,
          "set request timeout");
    check(karu_client_create(timeout_config, &timeout_client) == KARU_OK, "create timeout client");

    auto started = std::chrono::steady_clock::now();
    check(fetch(timeout_client, base + "/retry-after-deadline", 0, small) == KARU_TIMEOUT,
          "read retry delay respects request timeout");
    auto elapsed = std::chrono::steady_clock::now() - started;
    check(elapsed < std::chrono::seconds(2), "read retry deadline fails promptly");
    check(std::strstr(karu_last_error(), "request timeout") != nullptr,
          "read retry timeout detail");

    karu_locator* retrying_size = nullptr;
    check(karu_resolve((base + "/retry-after-deadline").c_str(), &retrying_size) == KARU_OK,
          "resolve retrying size endpoint");
    started = std::chrono::steady_clock::now();
    check(karu_client_size(timeout_client, retrying_size, &size) == KARU_TIMEOUT,
          "size retry delay respects request timeout");
    elapsed = std::chrono::steady_clock::now() - started;
    check(elapsed < std::chrono::seconds(2), "size retry deadline fails promptly");
    karu_locator_free(retrying_size);

    started = std::chrono::steady_clock::now();
    check(fetch(timeout_client, base + "/slow", 0, small) == KARU_TIMEOUT,
          "active read respects request timeout");
    elapsed = std::chrono::steady_clock::now() - started;
    check(elapsed < std::chrono::seconds(3), "active read timeout is bounded");

    karu_locator* slow_size = nullptr;
    check(karu_resolve((base + "/slow").c_str(), &slow_size) == KARU_OK,
          "resolve slow size endpoint");
    started = std::chrono::steady_clock::now();
    check(karu_client_size(timeout_client, slow_size, &size) == KARU_TIMEOUT,
          "active size probe respects request timeout");
    elapsed = std::chrono::steady_clock::now() - started;
    check(elapsed < std::chrono::seconds(3), "active size timeout is bounded");
    karu_locator_free(slow_size);
    karu_client_free(timeout_client);
    karu_config_free(timeout_config);

    check(fetch(client, base + "/missing", 0, small) == KARU_ERR_NOT_FOUND, "404 mapping");
    check(fetch(client, base + "/unauthorized", 0, small) == KARU_ERR_AUTH, "401 mapping");
    check(fetch(client, base + "/precondition", 0, small, "\"wrong\"") == KARU_ERR_PRECONDITION,
          "412 mapping");
    check(fetch(client, base + "/precondition", 0, small, "\"v1\"") == KARU_OK, "matching ETag");
    const karu_status bad_start = fetch(client, base + "/bad-range", 10, small);
    const std::string bad_start_detail = karu_last_error();
    check(bad_start == KARU_ERR_HTTP, "invalid Content-Range");
    check(bad_start_detail.find("Content-Range [0, 15] does not satisfy requested [10, 25]") !=
              std::string::npos,
          "Content-Range start mismatch is precise");
    const karu_status bad_end = fetch(client, base + "/bad-range-end", 10, small);
    const std::string bad_end_detail = karu_last_error();
    check(bad_end == KARU_ERR_HTTP, "oversized Content-Range rejected");
    check(bad_end_detail.find("Content-Range [10, 26] does not satisfy requested [10, 25]") !=
              std::string::npos,
          "Content-Range end mismatch is precise");

    check(fetch(client, base + "/missing?token=do-not-log", 0, small) == KARU_ERR_NOT_FOUND,
          "query-bearing 404 mapping");
    check(std::strstr(karu_last_error(), "do-not-log") == nullptr, "secret query redacted");
    check(std::strstr(karu_last_error(), "<redacted>") != nullptr, "redaction marker present");

    karu_config* strict_config = nullptr;
    karu_client* strict_client = nullptr;
    check(karu_config_create_empty(&strict_config) == KARU_OK, "create strict config");
    check(karu_config_set_option(strict_config, "KARU_RANGE_FALLBACK_LIMIT", "32") == KARU_OK,
          "set fallback limit");
    check(karu_client_create(strict_config, &strict_client) == KARU_OK, "create strict client");
    check(fetch(strict_client, base + "/ignore-range", 100, small) == KARU_ERR_HTTP,
          "large ignored Range rejected");
    karu_client_free(strict_client);
    karu_config_free(strict_config);

    karu_config* signed_config = nullptr;
    karu_client* signed_client = nullptr;
    check(karu_config_create_empty(&signed_config) == KARU_OK, "create signed config");
    check(karu_config_set_option(signed_config, "AWS_ACCESS_KEY_ID", "access") == KARU_OK,
          "set signed access key");
    check(karu_config_set_option(signed_config, "AWS_SECRET_ACCESS_KEY", "secret") == KARU_OK,
          "set signed secret");
    check(karu_config_set_option(signed_config, "AWS_S3_ENDPOINT", base.c_str()) == KARU_OK,
          "set signed endpoint");
    check(karu_client_create(signed_config, &signed_client) == KARU_OK, "create signed client");
    check(fetch(signed_client, "s3://bucket/signed-redirect", 0, small) == KARU_ERR_HTTP,
          "signed redirect not followed");
    check(fetch(signed_client, "s3://bucket/region", 0, small) == KARU_OK,
          "S3 region recovered from XML body");
    check(fetch(signed_client, "s3://bucket/late-region", 0, small) == KARU_OK,
          "S3 region recovered after a long XML message");
    check(fetch(signed_client, "s3://bucket/same-region", 0, small) == KARU_ERR_AUTH,
          "matching S3 region not retried");
    karu_client_free(signed_client);
    karu_config_free(signed_config);

    karu_config* refresh_config = nullptr;
    karu_client* refresh_client = nullptr;
    RefreshState refresh_state;
    check(karu_config_create_empty(&refresh_config) == KARU_OK, "create refresh config");
    check(karu_config_set_option(refresh_config, "GCS_ENDPOINT", base.c_str()) == KARU_OK,
          "set refresh endpoint");
    check(karu_config_set_credentials_provider(refresh_config, KARU_CREDENTIALS_GCS,
                                               refresh_credentials, &refresh_state,
                                               nullptr) == KARU_OK,
          "set refresh provider");
    check(karu_client_create(refresh_config, &refresh_client) == KARU_OK, "create refresh client");
    check(fetch(refresh_client, "gs://bucket/credential-refresh", 0, small) == KARU_OK,
          "expired credentials refreshed once");
    check(refresh_state.calls == 2, "credential provider called twice");
    karu_client_free(refresh_client);
    karu_config_free(refresh_config);

    std::array<unsigned char, 24> first{};
    std::array<unsigned char, 24> second{};
    const std::array<karu_req, 2> reads{{
        {object, 0, first.size(), first.data(), reinterpret_cast<void*>(1), nullptr},
        {object, first.size(), second.size(), second.data(), reinterpret_cast<void*>(2), nullptr},
    }};
    karu_batch* batch = nullptr;
    check(karu_client_submit(client, reads.data(), reads.size(), &batch) == KARU_OK,
          "submit coalesced batch");
    int completions = 0;
    while (batch != nullptr) {
        karu_done done{};
        const karu_status status = karu_batch_next(batch, &done, -1);
        if (status == KARU_END)
            break;
        check(status == KARU_OK && done.status == KARU_OK, "batch completion");
        ++completions;
    }
    check(completions == 2, "two batch completions");
    check_bytes(first, 0, "first coalesced contents");
    check_bytes(second, first.size(), "second coalesced contents");

    karu_batch_free(batch);

    // Different subfile locators over one outer object must share the same
    // physical range plan. Submit them in reverse physical order so tags, not
    // completion order, are what associate each result with its caller.
    karu_config* window_config = nullptr;
    karu_client* window_client = nullptr;
    check(karu_config_create_empty(&window_config) == KARU_OK, "create subfile config");
    check(karu_config_set_option(window_config, "KARU_COALESCE_GAP", "256") == KARU_OK,
          "set subfile coalescing");
    check(karu_config_set_option(window_config, "KARU_CONCURRENCY", "2") == KARU_OK,
          "set subfile concurrency");
    check(karu_config_set_option(window_config, "KARU_MAX_ATTEMPTS", "1") == KARU_OK,
          "set subfile attempts");
    check(karu_client_create(window_config, &window_client) == KARU_OK, "create subfile client");
    karu_config_free(window_config);

    karu_locator* earlier = nullptr;
    karu_locator* later = nullptr;
    const std::string outer = base + "/subfile-coalesced";
    check(karu_resolve(("/vsisubfile/100_100," + outer).c_str(), &earlier) == KARU_OK,
          "resolve earlier subfile");
    check(karu_resolve(("/vsisubfile/300_100," + outer).c_str(), &later) == KARU_OK,
          "resolve later subfile");

    std::array<unsigned char, 24> earlier_bytes{};
    std::array<unsigned char, 24> later_bytes{};
    const std::array<karu_req, 2> window_reads{{
        {later, 20, later_bytes.size(), later_bytes.data(), reinterpret_cast<void*>(2), nullptr},
        {earlier, 76, earlier_bytes.size(), earlier_bytes.data(), reinterpret_cast<void*>(1),
         nullptr},
    }};
    batch = nullptr;
    check(karu_client_submit(window_client, window_reads.data(), window_reads.size(), &batch) ==
              KARU_OK,
          "submit cross-subfile batch");
    bool saw_earlier = false;
    bool saw_later = false;
    while (batch != nullptr) {
        karu_done done{};
        const karu_status status = karu_batch_next(batch, &done, -1);
        if (status == KARU_END)
            break;
        check(status == KARU_OK && done.status == KARU_OK, "cross-subfile batch completion");
        if (done.tag == reinterpret_cast<void*>(1))
            saw_earlier = true;
        else if (done.tag == reinterpret_cast<void*>(2))
            saw_later = true;
        else
            check(false, "cross-subfile completion tag");
    }
    check(saw_earlier && saw_later, "cross-subfile completion tags");
    check_bytes(earlier_bytes, 176, "earlier subfile contents");
    check_bytes(later_bytes, 320, "later subfile contents");
    karu_batch_free(batch);
    karu_locator_free(earlier);
    karu_locator_free(later);

    // Prove cancellation while one coalesced HTTP transfer is active. The
    // loopback server exposes readiness only after seeing the expected merged
    // range, then deliberately holds that response open for four seconds.
    karu_locator* cancel_earlier = nullptr;
    karu_locator* cancel_later = nullptr;
    const std::string cancel_outer = base + "/cancel-coalesced";
    check(karu_resolve(("/vsisubfile/512_128," + cancel_outer).c_str(), &cancel_earlier) == KARU_OK,
          "resolve cancellable earlier subfile");
    check(karu_resolve(("/vsisubfile/768_128," + cancel_outer).c_str(), &cancel_later) == KARU_OK,
          "resolve cancellable later subfile");
    const std::array<karu_req, 2> cancel_reads{{
        {cancel_later, 0, 32, nullptr, reinterpret_cast<void*>(2), nullptr},
        {cancel_earlier, 64, 32, nullptr, reinterpret_cast<void*>(1), nullptr},
    }};
    batch = nullptr;
    check(karu_client_submit(window_client, cancel_reads.data(), cancel_reads.size(), &batch) ==
              KARU_OK,
          "submit cancellable coalesced batch");

    bool active = false;
    std::array<unsigned char, 1> readiness{};
    for (int attempt = 0; attempt < 100 && !active; ++attempt) {
        active = fetch(window_client, base + "/cancel-ready", 0, readiness) == KARU_OK;
        if (!active)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(active, "coalesced transfer became active");
    const auto cancel_at = std::chrono::steady_clock::now();
    karu_batch_free(batch);
    batch = nullptr;
    const auto cancel_time = std::chrono::steady_clock::now() - cancel_at;
    check(cancel_time < std::chrono::seconds(2), "active coalesced cancellation is prompt");

    karu_locator_free(cancel_earlier);
    karu_locator_free(cancel_later);
    karu_client_free(window_client);
    karu_locator_free(object);
    karu_client_free(client);
    return failures == 0 ? 0 : 1;
}
