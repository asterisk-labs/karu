#ifndef KARU_TRANSPORT_HPP
#define KARU_TRANSPORT_HPP

#include "../config.hpp"
#include "../locator.hpp"
#include "../request_builder.hpp"
#include "curl_types.hpp"
#include "transfer.hpp"

#include <cstdint>
#include <curl/curl.h>
#include <expected>
#include <string>
#include <string_view>

namespace karu::transport {

struct Failure {
    karu_status status;
    std::string detail;
};

[[nodiscard]] bool ensure_sink(Transfer& transfer) noexcept;

// Store one response-body chunk at the current transfer position. Merged HTTP
// transfers write only the intersections requested by their parts; gaps are
// consumed without being staged.
void store_payload(Transfer& transfer, const void* data, std::size_t size) noexcept;

[[nodiscard]] std::expected<void, std::string> configure(Transfer& transfer, CURLSH* share,
                                                         const ClientOptions& options);

// Keep a partial response only if the retry can request the same version.
void plan_resume(Transfer& transfer);
// Prefer the caller's condition over an ETag saved for resumption.
[[nodiscard]] std::string_view version_pin(const Transfer& transfer) noexcept;

[[nodiscard]] bool succeeded(const Transfer& transfer, CURLcode code) noexcept;
[[nodiscard]] bool retryable(const Transfer& transfer, CURLcode code) noexcept;
[[nodiscard]] Failure failure(const Transfer& transfer, CURLcode code);

// Limit DNS and connection failures separately from server-side retries.
// Connection and request timeouts still bound how long each attempt can take.
inline constexpr int kUnreachableAttempts = 3;
[[nodiscard]] bool unreachable(CURLcode code) noexcept;

struct ObjectInfo {
    std::uint64_t size = 0;
    // Strong entity tag with its quotes, or empty.
    std::string etag;
};

[[nodiscard]] std::expected<ObjectInfo, Failure> object_info(const Locator& locator, CURL* easy,
                                                             CURLSH* share,
                                                             RequestBuilder& request_builder,
                                                             const ClientOptions& options);

} // namespace karu::transport

#endif
