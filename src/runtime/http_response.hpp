#ifndef KARU_HTTP_RESPONSE_HPP
#define KARU_HTTP_RESPONSE_HPP

#include "transport.hpp"

#include <string>
#include <string_view>

namespace karu::transport::detail {

[[nodiscard]] std::string summarize(std::string_view body);
[[nodiscard]] karu_status status_for_http(long status) noexcept;
[[nodiscard]] bool transient(CURLcode code, long status) noexcept;

} // namespace karu::transport::detail

#endif
