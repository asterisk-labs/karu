#include "http_response.hpp"

#include "../text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>

namespace karu::transport {
namespace detail {

constexpr std::size_t kErrorSummaryLimit = 200;
constexpr int kMaximumRetryAfterSeconds = 60;

std::string summarize(std::string_view body) {
    std::string result;
    result.reserve(std::min(body.size(), kErrorSummaryLimit));
    bool pending_space = false;
    for (const char character : body) {
        if (static_cast<unsigned char>(character) <= ' ') {
            pending_space = true;
            continue;
        }
        if (pending_space && !result.empty())
            result.push_back(' ');
        pending_space = false;
        result.push_back(character);
        if (result.size() == kErrorSummaryLimit)
            break;
    }
    return result;
}

karu_status status_for_http(long status) noexcept {
    if (status == 401 || status == 403)
        return KARU_ERR_AUTH;
    if (status == 404)
        return KARU_ERR_NOT_FOUND;
    if (status == 412)
        return KARU_ERR_PRECONDITION;
    if (status == 416)
        return KARU_ERR_RANGE;
    return KARU_ERR_HTTP;
}

bool transient(CURLcode code, long status) noexcept {
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        return true;
    default:
        return status == 408 || status == 425 || status == 429 || (status >= 500 && status < 600);
    }
}

std::string s3_region(std::string_view body) {
    constexpr std::string_view open = "<Region>";
    constexpr std::string_view close = "</Region>";
    const std::size_t first = body.find(open);
    if (first == std::string_view::npos)
        return {};
    const std::size_t begin = first + open.size();
    const std::size_t end = body.find(close, begin);
    return end == std::string_view::npos ? std::string{}
                                         : std::string(body.substr(begin, end - begin));
}

bool credentials_expired(Backend backend, long status, std::string_view body) noexcept {
    if (status == 401 &&
        (backend == Backend::Gcs || backend == Backend::Azure || backend == Backend::Source)) {
        return true;
    }
    if (status != 403 || (backend != Backend::S3 && backend != Backend::Source))
        return false;
    return body.find("<Code>ExpiredToken</Code>") != std::string_view::npos ||
           body.find("<Code>InvalidToken</Code>") != std::string_view::npos ||
           body.find("<Code>RequestExpired</Code>") != std::string_view::npos;
}

bool credentials_expired(const Transfer& transfer) noexcept {
    return credentials_expired(
        transfer.locator->resolved.backend, transfer.http_status,
        std::string_view(transfer.http_buffers->error_body.data(), transfer.error_body_size));
}

int retry_after_seconds(std::string_view value, std::time_t now) noexcept {
    std::int64_t seconds = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
        if (seconds <= 0)
            return 0;
        return static_cast<int>(std::min<std::int64_t>(seconds, std::numeric_limits<int>::max()));
    }

    std::array<char, 128> copy{};
    if (value.size() >= copy.size())
        return 0;
    std::memcpy(copy.data(), value.data(), value.size());
    const std::time_t when = curl_getdate(copy.data(), nullptr);
    if (when == static_cast<std::time_t>(-1) || when <= now)
        return 0;
    const auto delay = static_cast<std::int64_t>(when - now);
    return static_cast<int>(std::min<std::int64_t>(delay, std::numeric_limits<int>::max()));
}

std::chrono::milliseconds retry_delay(int attempt, int retry_after, std::mt19937& random) {
    const int requested = std::clamp(retry_after, 0, kMaximumRetryAfterSeconds);
    if (requested > 0) {
        std::uniform_int_distribution<int> jitter(0, 1000);
        return std::chrono::seconds(requested) + std::chrono::milliseconds(jitter(random));
    }
    const int base = 100 << std::clamp(attempt, 0, 6);
    std::uniform_int_distribution<int> jitter(0, base);
    return std::chrono::milliseconds(base + jitter(random));
}

} // namespace detail

bool succeeded(const Transfer& transfer, CURLcode code) noexcept {
    const bool transport_ok = code == CURLE_OK || transfer.satisfied;
    if (!transport_ok || transfer.http_status < 200 || transfer.http_status >= 300)
        return false;
    if (transfer.http_status != 206)
        return true;
    if (!transfer.content_range_matches) {
        return false;
    }
    return transfer.received == transfer.content_range_end - transfer.content_range_start + 1;
}

bool retryable(const Transfer& transfer, CURLcode code) noexcept {
    if (detail::transient(code, transfer.http_status))
        return true;
    const std::string_view body(transfer.http_buffers->error_body.data(), transfer.error_body_size);
    return transfer.http_status == 400 && body.find("RequestTimeout") != std::string_view::npos;
}

Failure failure(const Transfer& transfer, CURLcode code) {
    const std::string url = redact_url(transfer.url());
    if (transfer.redirect_blocked) {
        return {KARU_ERR_HTTP,
                concat(url, ": cross-origin redirect blocked because KARU_HTTP_HEADERS is set")};
    }
    if (transfer.range_fallback_rejected) {
        return {KARU_ERR_HTTP,
                concat(url, ": server ignored Range; refusing to discard ", transfer.offset,
                       " bytes (limit ", transfer.range_fallback_limit, ")")};
    }
    if (transfer.http_status == 206 && !transfer.content_range_matches) {
        const std::uint64_t requested_end = transfer.offset + transfer.length - 1;
        if (!transfer.content_range_seen) {
            return {KARU_ERR_HTTP, concat(url, ": missing Content-Range for requested [",
                                          transfer.offset, ", ", requested_end, "]")};
        }
        if (!transfer.content_range_valid) {
            return {KARU_ERR_HTTP, concat(url, ": malformed Content-Range for requested [",
                                          transfer.offset, ", ", requested_end, "]")};
        }
        return {KARU_ERR_HTTP, concat(url, ": Content-Range [", transfer.content_range_start, ", ",
                                      transfer.content_range_end, "] does not satisfy requested [",
                                      transfer.offset, ", ", requested_end, "]")};
    }
    if (code != CURLE_OK && transfer.http_status >= 300 && transfer.http_status < 400 &&
        transfer.http->follow_redirects) {
        const char* message = transfer.http_buffers->error[0] != '\0'
                                  ? transfer.http_buffers->error.data()
                                  : curl_easy_strerror(code);
        return {KARU_ERR_NETWORK, concat(url, ": ", message)};
    }
    if (transfer.http_status >= 300) {
        std::string message = concat(url, ": HTTP ", transfer.http_status);
        const std::string body = detail::summarize(
            std::string_view(transfer.http_buffers->error_body.data(), transfer.error_body_size));
        if (!body.empty())
            message += ": " + body;
        return {detail::status_for_http(transfer.http_status), std::move(message)};
    }
    const char* message = transfer.http_buffers->error[0] != '\0'
                              ? transfer.http_buffers->error.data()
                              : curl_easy_strerror(code);
    return {KARU_ERR_NETWORK, concat(url, ": ", message)};
}

} // namespace karu::transport
