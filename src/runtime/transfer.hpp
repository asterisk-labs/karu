#ifndef KARU_TRANSFER_HPP
#define KARU_TRANSFER_HPP

#include "../buffer.hpp"
#include "../locator.hpp"
#include "batch.hpp"
#include "curl_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <string>
#include <vector>

namespace karu {

struct Request {
    const Locator* locator = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    void* buffer = nullptr;
    void* tag = nullptr;
    std::string if_match;
};

struct Part {
    std::uint64_t relative_offset = 0;
    std::uint64_t length = 0;
    void* buffer = nullptr;
    void* tag = nullptr;
    OwnedBuffer owned_buffer;
};

struct Transfer {
    BatchCore* batch = nullptr;
    Locator locator;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::vector<Part> parts;
    std::string if_match;

    bool scattered = false;
    std::byte* sink = nullptr;
    std::vector<std::byte> scratch;

    // Rebuilt for every attempt after the byte range is known. This keeps
    // renewable credentials and signatures out of the locator.
    std::string request_url;
    std::vector<std::pair<std::string, std::string>> request_headers;
    std::string region_hint;

    std::uint64_t received = 0;
    std::uint64_t skip = 0;
    std::int64_t body_length = -1;
    std::uint64_t consume = 0;
    bool satisfied = false;
    bool checked_status = false;
    long http_status = 0;
    bool content_range_seen = false;
    bool content_range_valid = false;
    std::uint64_t content_range_start = 0;
    std::string response_region;
    bool region_retried = false;
    int attempt = 0;

    int retry_after = 0;
    std::array<char, 512> error_body{};
    std::size_t error_body_size = 0;

    Easy easy;
    Slist headers;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};

    [[nodiscard]] const std::string& url() const noexcept { return request_url; }
};

} // namespace karu

#endif
