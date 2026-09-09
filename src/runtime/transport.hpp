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

namespace karu::transport {

struct Failure {
    karu_status status;
    std::string detail;
};

[[nodiscard]] bool ensure_sink(Transfer& transfer) noexcept;

[[nodiscard]] std::expected<void, std::string> configure(Transfer& transfer, CURLSH* share,
                                                         const ClientOptions& options);

[[nodiscard]] bool succeeded(const Transfer& transfer, CURLcode code) noexcept;
[[nodiscard]] bool retryable(const Transfer& transfer, CURLcode code) noexcept;
[[nodiscard]] Failure failure(const Transfer& transfer, CURLcode code);

[[nodiscard]] std::expected<std::uint64_t, Failure> size_of(const Locator& locator, CURL* easy,
                                                            CURLSH* share,
                                                            RequestBuilder& request_builder,
                                                            const ClientOptions& options);

} // namespace karu::transport

#endif
