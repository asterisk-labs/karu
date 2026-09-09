// End-to-end HTTP checks against tests/http_server.py.

#include "karu/karu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    std::array<unsigned char, 100> ignored{};
    check(fetch(client, base + "/ignore-range", 100, ignored) == KARU_OK, "server ignoring Range");
    check_bytes(ignored, 100, "ignored Range contents");

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
