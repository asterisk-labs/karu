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

int drain_success(karu_batch* batch, const char* message) {
    int completed = 0;
    for (;;) {
        karu_done done{};
        const karu_status status = karu_batch_next(batch, &done, -1);
        if (status == KARU_END)
            return completed;
        check(status == KARU_OK && done.status == KARU_OK, message);
        ++completed;
    }
}

struct RefreshState {
    karu_credentials_kind kind;
    const char* cache_prefix;
    int calls = 0;
};

struct SlowCredentialsState {
    std::atomic<int> active{0};
    std::atomic<bool> release{false};
};

karu_status refresh_credentials(void* data, karu_credentials_kind kind, const char*,
                                karu_credentials* out) {
    auto& state = *static_cast<RefreshState*>(data);
    ++state.calls;
    if (kind != state.kind)
        return KARU_ERR_CREDENTIALS;
    *out = karu_credentials{};
    const char* token = state.calls == 1 ? "stale" : "fresh";
    if (kind == KARU_CREDENTIALS_GCS || kind == KARU_CREDENTIALS_AZURE) {
        out->bearer_token = token;
    } else {
        out->access_key_id = "access";
        out->secret_access_key = "secret";
        out->session_token = token;
    }
    out->cache_prefix = state.cache_prefix;
    out->expires_at = static_cast<std::int64_t>(std::time(nullptr)) + 3600;
    return KARU_OK;
}

karu_status slow_credentials(void* data, karu_credentials_kind kind, const char*,
                             karu_credentials* out) {
    auto& state = *static_cast<SlowCredentialsState*>(data);
    state.active.fetch_add(1, std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!state.release.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    state.active.fetch_sub(1, std::memory_order_relaxed);
    if (kind != KARU_CREDENTIALS_GCS)
        return KARU_ERR_CREDENTIALS;
    *out = karu_credentials{};
    out->bearer_token = "token";
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

    std::array<unsigned char, 333> chunked{};
    check(fetch(client, base + "/chunked", 127, chunked) == KARU_OK, "chunked 206 range read");
    check_bytes(chunked, 127, "chunked 206 range contents");

    std::array<unsigned char, 1> user_agent_byte{};
    check(fetch(client, base + "/user-agent", 0, user_agent_byte) == KARU_OK,
          "default User-Agent reaches the server");

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
    const karu_req clipped_request{object, 4090, clipped.size(), clipped.data(), nullptr, nullptr};
    karu_batch* clipped_batch = nullptr;
    check(karu_client_submit(client, &clipped_request, 1, &clipped_batch) == KARU_OK,
          "submit range clipped at object end");
    karu_done clipped_done{};
    check(karu_batch_next(clipped_batch, &clipped_done, -1) == KARU_OK,
          "receive clipped range completion");
    check(clipped_done.status == KARU_ERR_RANGE, "range clipped at object end");
    check(clipped_done.got == 6, "clipped range reports its valid prefix");
    check_bytes(std::span(clipped).first(6), 4090, "clipped range contents");
    karu_batch_free(clipped_batch);

    std::array<unsigned char, 16> small{};
    check(fetch(client, base + "/redirect", 21, small) == KARU_OK, "same-origin redirect");
    check_bytes(small, 21, "redirect contents");
    check(fetch(client, base + "/redirect-cross-origin", 21, small) == KARU_OK,
          "headerless cross-origin redirect");
    check_bytes(small, 21, "headerless cross-origin redirect contents");
    check(fetch(client, base + "/redirect-ftp", 0, small) == KARU_ERR_NETWORK,
          "non-HTTP redirect rejected");
    karu_locator* redirected = nullptr;
    check(karu_resolve((base + "/redirect-ftp").c_str(), &redirected) == KARU_OK,
          "resolve non-HTTP redirect");
    check(karu_client_size(client, redirected, &size) == KARU_ERR_NETWORK,
          "non-HTTP size redirect rejected");
    karu_locator_free(redirected);

    karu_config* custom_header_config = nullptr;
    karu_client* custom_header_client = nullptr;
    check(karu_config_create_empty(&custom_header_config) == KARU_OK,
          "create custom-header config");
    check(karu_config_set_option(custom_header_config, "KARU_HTTP_HEADERS",
                                 "X-Karu-Secret: sentinel") == KARU_OK,
          "set custom redirect header");
    check(karu_client_create(custom_header_config, &custom_header_client) == KARU_OK,
          "create custom-header client");
    karu_config_free(custom_header_config);

    check(fetch(custom_header_client, base + "/redirect-same-origin-header", 21, small) == KARU_OK,
          "custom header retained on same-origin redirect");
    check(fetch(custom_header_client, base + "/redirect-relative-colon", 21, small) == KARU_OK,
          "colon in same-origin relative redirect");
    check_bytes(small, 21, "relative redirect with colon contents");
    check(fetch(custom_header_client, base + "/redirect-cross-origin", 21, small) == KARU_ERR_HTTP,
          "custom header blocks cross-origin redirect");
    check(std::strstr(karu_last_error(), "cross-origin redirect") != nullptr,
          "cross-origin redirect error is precise");

    karu_locator* cross_origin_size = nullptr;
    check(karu_resolve((base + "/redirect-cross-origin").c_str(), &cross_origin_size) == KARU_OK,
          "resolve custom-header size redirect");
    check(karu_client_size(custom_header_client, cross_origin_size, &size) == KARU_ERR_HTTP,
          "custom header blocks cross-origin size redirect");
    karu_locator_free(cross_origin_size);

    karu_locator* relative_colon_size = nullptr;
    check(karu_resolve((base + "/redirect-relative-colon").c_str(), &relative_colon_size) ==
              KARU_OK,
          "resolve relative redirect containing colon");
    check(karu_client_size(custom_header_client, relative_colon_size, &size) == KARU_OK &&
              size == 4096,
          "colon in same-origin relative size redirect");
    karu_locator_free(relative_colon_size);

    std::array<unsigned char, 1> leak_status{};
    check(fetch(custom_header_client, base + "/redirect-leak-status", 0, leak_status) == KARU_OK,
          "read redirect leak status");
    check(leak_status[0] == 0, "custom header never reached redirect target");
    karu_client_free(custom_header_client);
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

    karu_locator* dated_retry = nullptr;
    check(karu_resolve((base + "/retry-date-deadline").c_str(), &dated_retry) == KARU_OK,
          "resolve dated retry endpoint");
    check(karu_client_size(timeout_client, dated_retry, &size) == KARU_TIMEOUT,
          "size honors an HTTP-date Retry-After");
    karu_locator_free(dated_retry);

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

    karu_locator* truncated = nullptr;
    check(karu_resolve((base + "/truncated").c_str(), &truncated) == KARU_OK,
          "resolve truncated response");
    std::array<unsigned char, 32> truncated_bytes{};
    const karu_req truncated_request{
        truncated, 0, truncated_bytes.size(), truncated_bytes.data(), nullptr, nullptr};
    karu_batch* truncated_batch = nullptr;
    check(karu_client_submit(client, &truncated_request, 1, &truncated_batch) == KARU_OK,
          "submit truncated response");
    karu_done truncated_done{};
    check(karu_batch_next(truncated_batch, &truncated_done, -1) == KARU_OK,
          "receive truncated completion");
    check(truncated_done.status == KARU_ERR_NETWORK, "truncated response status");
    check(truncated_done.got == 0, "failed transfer exposes no readable prefix");
    karu_batch_free(truncated_batch);
    karu_locator_free(truncated);

    struct BackendCase {
        const char* root;
        const char* endpoint;
        const char* no_sign;
    };
    const std::array backend_cases{
        BackendCase{"s3://bucket", "AWS_S3_ENDPOINT", "AWS_NO_SIGN_REQUEST"},
        BackendCase{"gs://bucket", "GCS_ENDPOINT", "GCS_NO_SIGN_REQUEST"},
        BackendCase{"az://container", "AZURE_STORAGE_ENDPOINT", "AZURE_NO_SIGN_REQUEST"},
        BackendCase{"hf://datasets/org/repo", "HF_ENDPOINT", nullptr},
        BackendCase{"source://account/product", "SOURCE_ENDPOINT", nullptr},
    };
    for (const BackendCase& backend : backend_cases) {
        karu_config* backend_config = nullptr;
        karu_client* backend_client = nullptr;
        check(karu_config_create_empty(&backend_config) == KARU_OK, "create backend config");
        check(karu_config_set_option(backend_config, backend.endpoint, base.c_str()) == KARU_OK,
              "set backend endpoint");
        if (backend.no_sign != nullptr) {
            check(karu_config_set_option(backend_config, backend.no_sign, "YES") == KARU_OK,
                  "set anonymous backend");
        }
        check(karu_client_create(backend_config, &backend_client) == KARU_OK,
              "create backend client");
        karu_config_free(backend_config);

        const std::string root = backend.root;
        std::array<unsigned char, 37> backend_bytes{};
        check(fetch(backend_client, root + "/object", 111, backend_bytes) == KARU_OK,
              "backend range read");
        check_bytes(backend_bytes, 111, "backend range contents");

        karu_locator* backend_object = nullptr;
        check(karu_resolve((root + "/object").c_str(), &backend_object) == KARU_OK,
              "resolve backend object");
        std::uint64_t backend_size = 0;
        check(karu_client_size(backend_client, backend_object, &backend_size) == KARU_OK &&
                  backend_size == 4096,
              "backend size probe");
        karu_locator_free(backend_object);

        check(fetch(backend_client, root + "/missing", 0, backend_bytes) == KARU_ERR_NOT_FOUND,
              "backend 404 mapping");
        check(fetch(backend_client, root + "/precondition", 0, backend_bytes, "\"wrong\"") ==
                  KARU_ERR_PRECONDITION,
              "backend precondition failure");
        check(fetch(backend_client, root + "/precondition", 0, backend_bytes, "\"v1\"") == KARU_OK,
              "backend matching precondition");
        karu_client_free(backend_client);
    }

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

    struct RefreshCase {
        const char* root;
        const char* endpoint;
        karu_credentials_kind kind;
        const char* cache_prefix;
    };
    const std::array refresh_cases{
        RefreshCase{"s3://bucket", "AWS_S3_ENDPOINT", KARU_CREDENTIALS_AWS, "/vsis3/bucket/"},
        RefreshCase{"gs://bucket", "GCS_ENDPOINT", KARU_CREDENTIALS_GCS, "/vsigs/bucket/"},
        RefreshCase{"az://container", "AZURE_STORAGE_ENDPOINT", KARU_CREDENTIALS_AZURE,
                    "/vsiaz/container/"},
        RefreshCase{"source://account/product", "SOURCE_ENDPOINT", KARU_CREDENTIALS_SOURCE,
                    "/vsisource/account/product/"},
    };
    for (const RefreshCase& backend : refresh_cases) {
        karu_config* refresh_config = nullptr;
        karu_client* refresh_client = nullptr;
        RefreshState refresh_state{backend.kind, backend.cache_prefix};
        check(karu_config_create_empty(&refresh_config) == KARU_OK, "create refresh config");
        check(karu_config_set_option(refresh_config, backend.endpoint, base.c_str()) == KARU_OK,
              "set refresh endpoint");
        check(karu_config_set_credentials_provider(refresh_config, backend.kind,
                                                   refresh_credentials, &refresh_state,
                                                   nullptr) == KARU_OK,
              "set refresh provider");
        check(karu_client_create(refresh_config, &refresh_client) == KARU_OK,
              "create refresh client");
        check(fetch(refresh_client, std::string(backend.root) + "/credential-refresh", 0, small) ==
                  KARU_OK,
              "expired credentials refreshed once");
        check(refresh_state.calls == 2, "credential provider called twice");
        karu_client_free(refresh_client);
        karu_config_free(refresh_config);
    }

    karu_config* slow_credentials_config = nullptr;
    karu_client* slow_credentials_client = nullptr;
    SlowCredentialsState slow_credentials_state;
    check(karu_config_create_empty(&slow_credentials_config) == KARU_OK,
          "create slow credentials config");
    check(karu_config_set_option(slow_credentials_config, "GCS_ENDPOINT", base.c_str()) == KARU_OK,
          "set slow credentials endpoint");
    check(karu_config_set_option(slow_credentials_config, "KARU_CONCURRENCY", "4") == KARU_OK,
          "set slow credentials concurrency");
    check(karu_config_set_credentials_provider(slow_credentials_config, KARU_CREDENTIALS_GCS,
                                               slow_credentials, &slow_credentials_state,
                                               nullptr) == KARU_OK,
          "set slow credentials provider");
    check(karu_client_create(slow_credentials_config, &slow_credentials_client) == KARU_OK,
          "create slow credentials client");
    karu_config_free(slow_credentials_config);

    std::array<karu_locator*, 4> slow_locators{};
    std::array<std::array<unsigned char, 1>, 4> slow_buffers{};
    std::array<karu_req, 4> slow_requests{};
    for (std::size_t index = 0; index < slow_locators.size(); ++index) {
        const std::string uri = "gs://bucket/slow-credentials-" + std::to_string(index);
        check(karu_resolve(uri.c_str(), &slow_locators[index]) == KARU_OK,
              "resolve slow credential object");
        slow_requests[index] =
            karu_req{slow_locators[index], 0, 1, slow_buffers[index].data(), nullptr, nullptr};
    }
    karu_batch* slow_batch = nullptr;
    check(karu_client_submit(slow_credentials_client, slow_requests.data(), slow_requests.size(),
                             &slow_batch) == KARU_OK,
          "submit slow credential batch");
    const auto credentials_wait_started = std::chrono::steady_clock::now();
    while (slow_credentials_state.active.load(std::memory_order_relaxed) < 4 &&
           std::chrono::steady_clock::now() - credentials_wait_started < std::chrono::seconds(1)) {
        std::this_thread::yield();
    }
    const bool credentials_are_blocked =
        slow_credentials_state.active.load(std::memory_order_relaxed) == 4;
    check(credentials_are_blocked, "independent credential loads run concurrently");

    if (credentials_are_blocked) {
        started = std::chrono::steady_clock::now();
        check(fetch(slow_credentials_client, base + "/object", 0, small) == KARU_OK,
              "HTTP read bypasses cloud credential loading");
        elapsed = std::chrono::steady_clock::now() - started;
        check(elapsed < std::chrono::seconds(1),
              "slow credential providers do not block HTTP transport");
    }
    slow_credentials_state.release.store(true, std::memory_order_release);

    int slow_completions = 0;
    while (slow_batch != nullptr) {
        karu_done done{};
        const karu_status status = karu_batch_next(slow_batch, &done, -1);
        if (status == KARU_END)
            break;
        check(status == KARU_OK && done.status == KARU_OK, "slow credential batch completion");
        ++slow_completions;
    }
    check(slow_completions == 4, "all slow credential reads complete");
    karu_batch_free(slow_batch);
    for (karu_locator* locator : slow_locators)
        karu_locator_free(locator);
    karu_client_free(slow_credentials_client);

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

    karu_locator* separate_object = nullptr;
    check(karu_resolve((base + "/submit-separate").c_str(), &separate_object) == KARU_OK,
          "resolve per-submit fixture");
    std::array<unsigned char, 24> separate_first{};
    std::array<unsigned char, 24> separate_second{};
    const std::array<karu_req, 2> separate_reads{{
        {separate_object, 0, separate_first.size(), separate_first.data(), nullptr, nullptr},
        {separate_object, separate_first.size(), separate_second.size(), separate_second.data(),
         nullptr, nullptr},
    }};
    const karu_submit_options separate_options{sizeof(karu_submit_options), 0, KARU_INHERIT,
                                               KARU_INHERIT, KARU_INHERIT};
    batch = nullptr;
    check(karu_client_submit_with(client, separate_reads.data(), separate_reads.size(),
                                  &separate_options, &batch) == KARU_OK,
          "disable coalescing for one submit");
    check(drain_success(batch, "separate range completion") == 2, "both separate ranges complete");
    check_bytes(separate_first, 0, "first separate contents");
    check_bytes(separate_second, separate_first.size(), "second separate contents");
    karu_batch_free(batch);

    separate_first.fill(0);
    const karu_req separate_fetch{separate_object,       0,       separate_first.size(),
                                  separate_first.data(), nullptr, nullptr};
    check(karu_client_fetch_with(client, &separate_fetch, 1, &separate_options) == KARU_OK,
          "fetch with per-call options");
    check_bytes(separate_first, 0, "per-call fetch contents");
    karu_locator_free(separate_object);

    // Different subfile locators over one outer object must share the same
    // physical range plan. Submit them in reverse physical order so tags, not
    // completion order, are what associate each result with its caller.
    karu_config* window_config = nullptr;
    karu_client* window_client = nullptr;
    check(karu_config_create_empty(&window_config) == KARU_OK, "create subfile config");
    check(karu_config_set_option(window_config, "KARU_COALESCE_GAP", "0") == KARU_OK,
          "disable client-wide subfile coalescing");
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
    const karu_submit_options window_options{sizeof(karu_submit_options), 256, KARU_INHERIT,
                                             KARU_INHERIT, KARU_INHERIT};
    batch = nullptr;
    check(karu_client_submit_with(window_client, window_reads.data(), window_reads.size(),
                                  &window_options, &batch) == KARU_OK,
          "submit cross-subfile batch with local planner options");
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

    // One response is delivered in uneven chunks and must populate every
    // requested intersection, including duplicate and overlapping ranges,
    // without retaining the five-kilobyte hole.
    karu_locator* scatter_object = nullptr;
    check(karu_resolve((base + "/scatter-chunks").c_str(), &scatter_object) == KARU_OK,
          "resolve scatter fixture");
    std::array<unsigned char, 40000> scatter_first{};
    std::array<unsigned char, 40000> scatter_duplicate{};
    std::array<unsigned char, 35000> scatter_overlap{};
    std::array<unsigned char, 20000> scatter_final{};
    const std::array<karu_req, 4> scatter_reads{{
        {scatter_object, 1000, scatter_first.size(), scatter_first.data(), nullptr, nullptr},
        {scatter_object, 1000, scatter_duplicate.size(), scatter_duplicate.data(), nullptr,
         nullptr},
        {scatter_object, 20000, scatter_overlap.size(), scatter_overlap.data(), nullptr, nullptr},
        {scatter_object, 60000, scatter_final.size(), scatter_final.data(), nullptr, nullptr},
    }};
    const karu_submit_options scatter_options{sizeof(karu_submit_options), 8192, KARU_INHERIT,
                                              KARU_INHERIT, KARU_INHERIT};
    batch = nullptr;
    check(karu_client_submit_with(window_client, scatter_reads.data(), scatter_reads.size(),
                                  &scatter_options, &batch) == KARU_OK,
          "submit scattered range shapes");
    check(drain_success(batch, "scattered range completion") == 4, "all scattered ranges complete");
    check_bytes(scatter_first, 1000, "first scattered contents");
    check_bytes(scatter_duplicate, 1000, "duplicate scattered contents");
    check_bytes(scatter_overlap, 20000, "overlapping scattered contents");
    check_bytes(scatter_final, 60000, "final scattered contents");
    karu_batch_free(batch);
    karu_locator_free(scatter_object);

    karu_locator* ignored_scatter = nullptr;
    check(karu_resolve((base + "/ignore-range").c_str(), &ignored_scatter) == KARU_OK,
          "resolve ignored Range scatter fixture");
    std::array<unsigned char, 24> ignored_first{};
    std::array<unsigned char, 24> ignored_second{};
    const std::array<karu_req, 2> ignored_reads{{
        {ignored_scatter, 100, ignored_first.size(), ignored_first.data(), nullptr, nullptr},
        {ignored_scatter, 200, ignored_second.size(), ignored_second.data(), nullptr, nullptr},
    }};
    batch = nullptr;
    check(karu_client_submit_with(window_client, ignored_reads.data(), ignored_reads.size(),
                                  &scatter_options, &batch) == KARU_OK,
          "submit scattered ignored Range fallback");
    check(drain_success(batch, "scattered ignored Range completion") == 2,
          "both ignored Range parts complete");
    check_bytes(ignored_first, 100, "first ignored Range part");
    check_bytes(ignored_second, 200, "second ignored Range part");
    karu_batch_free(batch);
    karu_locator_free(ignored_scatter);

    karu_locator* retry_scatter = nullptr;
    check(karu_resolve((base + "/scatter-retry").c_str(), &retry_scatter) == KARU_OK,
          "resolve scatter retry fixture");
    std::array<unsigned char, 20> retry_first{};
    std::array<unsigned char, 20> retry_second{};
    const std::array<karu_req, 2> retry_reads{{
        {retry_scatter, 300, retry_first.size(), retry_first.data(), nullptr, nullptr},
        {retry_scatter, 360, retry_second.size(), retry_second.data(), nullptr, nullptr},
    }};
    batch = nullptr;
    check(karu_client_submit_with(client, retry_reads.data(), retry_reads.size(), &scatter_options,
                                  &batch) == KARU_OK,
          "submit scattered retry");
    check(drain_success(batch, "scattered retry completion") == 2, "both retry parts complete");
    check_bytes(retry_first, 300, "first retry part");
    check_bytes(retry_second, 360, "second retry part");
    karu_batch_free(batch);
    karu_locator_free(retry_scatter);

    std::array<unsigned char, 16> short_first{};
    std::array<unsigned char, 16> short_second{};
    const std::array<karu_req, 2> short_reads{{
        {object, 4080, short_first.size(), short_first.data(), reinterpret_cast<void*>(1), nullptr},
        {object, 4090, short_second.size(), short_second.data(), reinterpret_cast<void*>(2),
         nullptr},
    }};
    batch = nullptr;
    check(karu_client_submit_with(window_client, short_reads.data(), short_reads.size(),
                                  &scatter_options, &batch) == KARU_OK,
          "submit scattered short read");
    bool short_first_ok = false;
    bool short_second_clipped = false;
    for (;;) {
        karu_done done{};
        const karu_status status = karu_batch_next(batch, &done, -1);
        if (status == KARU_END)
            break;
        check(status == KARU_OK, "receive scattered short completion");
        if (done.tag == reinterpret_cast<void*>(1)) {
            short_first_ok = done.status == KARU_OK && done.got == short_first.size();
        } else if (done.tag == reinterpret_cast<void*>(2)) {
            short_second_clipped = done.status == KARU_ERR_RANGE && done.got == 6;
        }
    }
    check(short_first_ok, "complete part before object end");
    check(short_second_clipped, "partial part reports readable prefix");
    check_bytes(short_first, 4080, "complete short-read part contents");
    check_bytes(std::span(short_second).first(6), 4090, "partial short-read part contents");
    karu_batch_free(batch);

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
    check(karu_client_submit_with(window_client, cancel_reads.data(), cancel_reads.size(),
                                  &window_options, &batch) == KARU_OK,
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
