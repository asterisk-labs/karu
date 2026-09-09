// End-to-end HTTP checks against tests/http_server.py.

#include "karu/karu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <span>
#include <string>

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

    karu_locator* unknown_size = nullptr;
    check(karu_resolve((base + "/unknown-size").c_str(), &unknown_size) == KARU_OK,
          "resolve unknown size");
    check(karu_client_size(client, unknown_size, &size) == KARU_ERR_HTTP,
          "unknown Content-Range total rejected");
    karu_locator_free(unknown_size);

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
    check(fetch(client, base + "/missing", 0, small) == KARU_ERR_NOT_FOUND, "404 mapping");
    check(fetch(client, base + "/unauthorized", 0, small) == KARU_ERR_AUTH, "401 mapping");
    check(fetch(client, base + "/precondition", 0, small, "\"wrong\"") == KARU_ERR_PRECONDITION,
          "412 mapping");
    check(fetch(client, base + "/precondition", 0, small, "\"v1\"") == KARU_OK, "matching ETag");
    check(fetch(client, base + "/bad-range", 10, small) == KARU_ERR_HTTP, "invalid Content-Range");
    check(fetch(client, base + "/bad-range-end", 10, small) == KARU_ERR_HTTP,
          "oversized Content-Range rejected");

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
    check(fetch(signed_client, "s3://bucket/same-region", 0, small) == KARU_ERR_AUTH,
          "matching S3 region not retried");
    karu_client_free(signed_client);
    karu_config_free(signed_config);

    karu_config* refresh_config = nullptr;
    karu_client* refresh_client = nullptr;
    RefreshState refresh_state;
    check(karu_config_create_empty(&refresh_config) == KARU_OK, "create refresh config");
    check(karu_config_set_option(refresh_config, "CPL_GS_ENDPOINT", base.c_str()) == KARU_OK,
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
    karu_locator_free(object);
    karu_client_free(client);
    return failures == 0 ? 0 : 1;
}
