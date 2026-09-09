#ifndef KARU_HTTP_RESPONSE_HPP
#define KARU_HTTP_RESPONSE_HPP

#include "transport.hpp"

#include <string>
#include <string_view>

namespace karu::transport::detail {

[[nodiscard]] std::string summarize(std::string_view body);
[[nodiscard]] karu_status status_for_http(long status) noexcept;
[[nodiscard]] bool transient(CURLcode code, long status) noexcept;
[[nodiscard]] std::string s3_region(std::string_view body);
[[nodiscard]] bool credentials_expired(Backend backend, long status,
                                       std::string_view body) noexcept;
[[nodiscard]] bool credentials_expired(const Transfer& transfer) noexcept;

} // namespace karu::transport::detail

#endif
