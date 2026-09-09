#include "transport.hpp"

#include "../text.hpp"
#include "http_response.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>
#include <random>
#include <string_view>
#include <thread>

namespace karu::transport {
namespace {

constexpr std::uint64_t kDrainLimit = 256u << 10;

long curl_http_version(HttpVersion version) noexcept {
    switch (version) {
    case HttpVersion::Automatic:
        return CURL_HTTP_VERSION_NONE;
    case HttpVersion::Http2Tls:
        return CURL_HTTP_VERSION_2TLS;
    case HttpVersion::Http2PriorKnowledge:
        return CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE;
    case HttpVersion::Http1_1:
        return CURL_HTTP_VERSION_1_1;
    }
    return CURL_HTTP_VERSION_1_1;
}

bool uses_http2(HttpVersion version) noexcept {
    return version == HttpVersion::Http2Tls || version == HttpVersion::Http2PriorKnowledge;
}

std::optional<std::size_t> callback_size(std::size_t size, std::size_t count) noexcept {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return std::nullopt;
    }
    return size * count;
}

bool header_name_is(std::string_view line, std::string_view name) noexcept {
    if (line.size() < name.size())
        return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto left = static_cast<unsigned char>(line[index]);
        const auto right = static_cast<unsigned char>(name[index]);
        if (std::tolower(left) != std::tolower(right))
            return false;
    }
    return true;
}

std::string_view header_value(std::string_view line, std::string_view name) noexcept {
    std::string_view value = line.substr(name.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' ||
                              value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

std::time_t parse_http_date(std::string_view value) noexcept {
    std::array<char, 128> copy{};
    if (value.size() >= copy.size())
        return static_cast<std::time_t>(-1);
    std::memcpy(copy.data(), value.data(), value.size());
    return curl_getdate(copy.data(), nullptr);
}

bool parse_u64(std::string_view text, std::uint64_t& value) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_content_range(std::string_view value, std::uint64_t& first, std::uint64_t& last,
                         std::uint64_t& total, bool& have_total) noexcept {
    constexpr std::string_view prefix = "bytes ";
    if (!value.starts_with(prefix))
        return false;
    value.remove_prefix(prefix.size());
    const auto dash = value.find('-');
    const auto slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash == 0 ||
        dash > slash) {
        return false;
    }
    if (!parse_u64(value.substr(0, dash), first) ||
        !parse_u64(value.substr(dash + 1, slash - dash - 1), last) || last < first) {
        return false;
    }
    const std::string_view total_text = value.substr(slash + 1);
    have_total = total_text != "*";
    if (have_total && (!parse_u64(total_text, total) || total == 0 || last >= total))
        return false;
    return !total_text.empty();
}

bool parse_unsatisfied_content_range(std::string_view value, std::uint64_t& total) noexcept {
    constexpr std::string_view prefix = "bytes */";
    return value.starts_with(prefix) && parse_u64(value.substr(prefix.size()), total);
}

bool valid_content_range(const Transfer& transfer) noexcept {
    if (!transfer.content_range_seen || !transfer.content_range_valid ||
        transfer.content_range_start != transfer.offset) {
        return false;
    }
    const std::uint64_t requested_end = transfer.offset + transfer.length - 1;
    if (transfer.content_range_end > requested_end)
        return false;
    return transfer.content_range_end == requested_end ||
           (transfer.content_range_has_total &&
            transfer.content_range_end + 1 == transfer.content_range_total);
}

template <std::size_t Capacity>
void capture_error_body(std::array<char, Capacity>& destination, std::size_t& used,
                        const char* data, std::size_t size) noexcept {
    const std::size_t take = std::min(destination.size() - used, size);
    if (take > 0) {
        std::memcpy(destination.data() + used, data, take);
        used += take;
    }
}

std::size_t write_callback(char* data, std::size_t size, std::size_t count,
                           void* userdata) noexcept {
    const auto incoming = callback_size(size, count);
    if (!incoming)
        return 0;
    auto& transfer = *static_cast<Transfer*>(userdata);
    std::size_t remaining = *incoming;

    if (transfer.satisfied)
        return *incoming;

    if (!transfer.checked_status) {
        transfer.checked_status = true;
        curl_easy_getinfo(transfer.easy.get(), CURLINFO_RESPONSE_CODE, &transfer.http_status);
        if (transfer.http_status == 206 && !valid_content_range(transfer)) {
            return 0;
        }
        if (transfer.http_status == 200 && transfer.offset > 0) {
            if (transfer.offset > transfer.range_fallback_limit) {
                transfer.range_fallback_rejected = true;
                return 0;
            }
            transfer.skip = transfer.offset;
        }
        curl_off_t body_length = -1;
        curl_easy_getinfo(transfer.easy.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &body_length);
        transfer.body_length = body_length;
        transfer.consume =
            transfer.skip > std::numeric_limits<std::uint64_t>::max() - transfer.length
                ? std::numeric_limits<std::uint64_t>::max()
                : transfer.skip + transfer.length;
    }

    if (transfer.http_status >= 300 && transfer.http_status < 400 &&
        transfer.http->follow_redirects) {
        return *incoming;
    }

    if (transfer.http_status >= 300) {
        capture_error_body(transfer.http_buffers->error_body, transfer.error_body_size, data,
                           *incoming);
        if (*incoming > kDrainLimit - std::min(kDrainLimit, transfer.error_body_received)) {
            return 0;
        }
        transfer.error_body_received += *incoming;
        return *incoming;
    }

    if (transfer.skip > 0) {
        const auto drop =
            static_cast<std::size_t>(std::min<std::uint64_t>(transfer.skip, remaining));
        data += drop;
        remaining -= drop;
        transfer.skip -= drop;
    }
    if (remaining == 0)
        return *incoming;

    const std::uint64_t room = transfer.length - transfer.received;
    const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(room, remaining));
    if (take > 0) {
        std::memcpy(transfer.sink + transfer.received, data, take);
        transfer.received += take;
    }

    if (transfer.received == transfer.length) {
        transfer.satisfied = true;
        std::uint64_t excess = std::numeric_limits<std::uint64_t>::max();
        if (transfer.body_length >= 0) {
            const auto body = static_cast<std::uint64_t>(transfer.body_length);
            excess = body > transfer.consume ? body - transfer.consume : 0;
        } else if (take == remaining && transfer.http_status != 200) {
            excess = 0;
        }
        if (excess > kDrainLimit)
            return 0;
    }
    return *incoming;
}

std::size_t header_callback(char* data, std::size_t size, std::size_t count,
                            void* userdata) noexcept {
    const auto bytes = callback_size(size, count);
    if (!bytes)
        return 0;
    auto& transfer = *static_cast<Transfer*>(userdata);
    const std::string_view line(data, *bytes);

    if (line.starts_with("HTTP/")) {
        transfer.received = 0;
        transfer.skip = 0;
        transfer.body_length = -1;
        transfer.consume = 0;
        transfer.satisfied = false;
        transfer.checked_status = false;
        transfer.http_status = 0;
        transfer.content_range_seen = false;
        transfer.content_range_valid = false;
        transfer.content_range_matches = false;
        transfer.content_range_has_total = false;
        transfer.range_fallback_rejected = false;
        transfer.response_region.clear();
        transfer.retry_after = 0;
        transfer.error_body_size = 0;
        transfer.error_body_received = 0;
    } else if (header_name_is(line, "content-range:")) {
        transfer.content_range_seen = true;
        transfer.content_range_valid =
            parse_content_range(header_value(line, "content-range:"), transfer.content_range_start,
                                transfer.content_range_end, transfer.content_range_total,
                                transfer.content_range_has_total);
        if (transfer.content_range_valid)
            transfer.content_range_matches = valid_content_range(transfer);
    } else if (header_name_is(line, "x-amz-bucket-region:")) {
        transfer.response_region = std::string(header_value(line, "x-amz-bucket-region:"));
    } else if (header_name_is(line, "retry-after:")) {
        const std::string_view value = header_value(line, "retry-after:");
        int seconds = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
            transfer.retry_after = seconds;
        } else {
            const std::time_t when = parse_http_date(value);
            const std::time_t now = std::time(nullptr);
            if (when != static_cast<std::time_t>(-1) && when > now &&
                when - now <= std::numeric_limits<int>::max()) {
                transfer.retry_after = static_cast<int>(when - now);
            }
        }
    }
    return *bytes;
}

std::expected<Slist, std::string>
build_headers(const std::vector<std::pair<std::string, std::string>>& values,
              std::string_view range) {
    Slist headers;
    auto append = [&](std::string_view name,
                      std::string_view value) -> std::expected<void, std::string> {
        if (name.empty() || name.find_first_of(":\r\n") != std::string_view::npos ||
            value.find_first_of("\r\n") != std::string_view::npos) {
            return std::unexpected("invalid HTTP header name or value");
        }
        std::string line;
        line.reserve(name.size() + value.size() + 2);
        line.append(name);
        line.append(": ");
        line.append(value);
        curl_slist* appended = curl_slist_append(headers.get(), line.c_str());
        if (appended == nullptr)
            return std::unexpected("out of memory building HTTP headers");
        static_cast<void>(headers.release());
        headers.reset(appended);
        return {};
    };
    for (const auto& [name, value] : values) {
        if (auto added = append(name, value); !added)
            return std::unexpected(added.error());
    }
    if (auto added = append("Range", range); !added)
        return std::unexpected(added.error());
    return headers;
}

template <typename Value>
std::expected<void, std::string> set_option(CURL* easy, CURLoption option, Value value) {
    const CURLcode code = curl_easy_setopt(easy, option, value);
    if (code == CURLE_OK)
        return {};
    if (const curl_easyoption* details = curl_easy_option_by_id(option); details != nullptr) {
        return std::unexpected(concat("CURLOPT_", details->name, ": ", curl_easy_strerror(code)));
    }
    return std::unexpected(
        concat("curl option ", static_cast<long>(option), ": ", curl_easy_strerror(code)));
}

template <typename Value, typename... Rest>
std::expected<void, std::string> set_options(CURL* easy, CURLoption option, Value value,
                                             Rest... rest) {
    if (auto result = set_option(easy, option, value); !result)
        return result;
    if constexpr (sizeof...(rest) == 0) {
        return {};
    } else {
        return set_options(easy, rest...);
    }
}

std::expected<void, std::string> restrict_to_http(CURL* easy) {
#if LIBCURL_VERSION_NUM >= 0x075500
    return set_options(easy, CURLOPT_PROTOCOLS_STR, "http,https", CURLOPT_REDIR_PROTOCOLS_STR,
                       "http,https");
#else
    constexpr long protocols = CURLPROTO_HTTP | CURLPROTO_HTTPS;
    return set_options(easy, CURLOPT_PROTOCOLS, protocols, CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
}

std::expected<void, std::string> apply_http_options(CURL* easy, const HttpRequestOptions& options) {
    if (auto result = set_options(easy, CURLOPT_FOLLOWLOCATION, options.follow_redirects ? 1L : 0L,
                                  CURLOPT_HTTP_VERSION, curl_http_version(options.version),
                                  CURLOPT_PIPEWAIT, uses_http2(options.version) ? 1L : 0L);
        !result) {
        return result;
    }
    const auto apply_string = [&](CURLoption option, const std::string& value) {
        return value.empty() ? std::expected<void, std::string>{}
                             : set_option(easy, option, value.c_str());
    };
    if (auto result = apply_string(CURLOPT_CAINFO, options.ca_bundle); !result)
        return result;
    if (auto result = apply_string(CURLOPT_CAPATH, options.ca_path); !result)
        return result;
    if (auto result = apply_string(CURLOPT_PROXY, options.proxy); !result)
        return result;
    if (auto result = apply_string(CURLOPT_PROXYUSERPWD, options.proxy_user_password); !result)
        return result;
    return apply_string(CURLOPT_USERAGENT, options.user_agent);
}

struct SizeState {
    CURL* easy = nullptr;
    std::uint64_t total = 0;
    std::size_t body_received = 0;
    bool have_total = false;
    bool cut_short = false;
    bool body_exceeded_range = false;
    bool content_range_valid = false;
    bool unsatisfied_content_range_valid = false;
    bool follow_redirects = true;
    int retry_after = 0;
    std::string response_region;
    std::array<char, kErrorBodyCapacity> error_body{};
    std::size_t error_body_size = 0;
    std::uint64_t error_body_received = 0;
};

std::size_t size_body_callback(char* data, std::size_t size, std::size_t count,
                               void* userdata) noexcept {
    const auto bytes = callback_size(size, count);
    if (!bytes)
        return 0;
    auto& state = *static_cast<SizeState*>(userdata);
    long status = 0;
    curl_easy_getinfo(state.easy, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 300 && status < 400 && state.follow_redirects)
        return *bytes;
    if (status >= 300) {
        capture_error_body(state.error_body, state.error_body_size, data, *bytes);
        if (*bytes > kDrainLimit - std::min(kDrainLimit, state.error_body_received))
            return 0;
        state.error_body_received += *bytes;
        return *bytes;
    }
    if (status == 206 && state.content_range_valid) {
        if (*bytes > 1 - std::min<std::size_t>(1, state.body_received)) {
            state.body_exceeded_range = true;
            return 0;
        }
        state.body_received += *bytes;
        return *bytes;
    }
    state.cut_short = true;
    return 0;
}

std::size_t size_header_callback(char* data, std::size_t size, std::size_t count,
                                 void* userdata) noexcept {
    const auto bytes = callback_size(size, count);
    if (!bytes)
        return 0;
    auto& state = *static_cast<SizeState*>(userdata);
    const std::string_view line(data, *bytes);

    if (line.starts_with("HTTP/")) {
        state.total = 0;
        state.body_received = 0;
        state.have_total = false;
        state.cut_short = false;
        state.body_exceeded_range = false;
        state.content_range_valid = false;
        state.unsatisfied_content_range_valid = false;
        state.retry_after = 0;
        state.response_region.clear();
        state.error_body_size = 0;
    } else if (header_name_is(line, "content-range:")) {
        const std::string_view value = header_value(line, "content-range:");
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        std::uint64_t parsed_total = 0;
        bool parsed_have_total = false;
        state.content_range_valid =
            parse_content_range(value, first, last, parsed_total, parsed_have_total) &&
            first == 0 && last == 0;
        if (state.content_range_valid && parsed_have_total) {
            state.total = parsed_total;
            state.have_total = true;
            return *bytes;
        }
        std::uint64_t unsatisfied_total = 0;
        state.unsatisfied_content_range_valid =
            parse_unsatisfied_content_range(value, unsatisfied_total);
        if (state.unsatisfied_content_range_valid) {
            state.total = unsatisfied_total;
            state.have_total = true;
        }
    } else if (header_name_is(line, "x-amz-bucket-region:")) {
        state.response_region = std::string(header_value(line, "x-amz-bucket-region:"));
    } else if (header_name_is(line, "retry-after:")) {
        const auto value = header_value(line, "retry-after:");
        int seconds = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
            state.retry_after = seconds;
        }
    }
    return *bytes;
}

} // namespace

bool ensure_sink(Transfer& transfer) noexcept {
    if (transfer.sink != nullptr)
        return true;
    if (!transfer.scattered) {
        transfer.sink = static_cast<std::byte*>(transfer.parts.front().buffer);
        return true;
    }
    if (transfer.length > std::numeric_limits<std::size_t>::max())
        return false;
    try {
        transfer.scratch.resize(static_cast<std::size_t>(transfer.length));
    } catch (...) {
        return false;
    }
    transfer.sink = transfer.scratch.data();
    return true;
}

std::expected<void, std::string> configure(Transfer& transfer, CURLSH* share,
                                           const ClientOptions& options) {
    if (!transfer.http || !transfer.http_buffers)
        return std::unexpected("HTTP transfer state is not initialized");
    transfer.received = 0;
    transfer.skip = 0;
    transfer.body_length = -1;
    transfer.consume = 0;
    transfer.satisfied = false;
    transfer.checked_status = false;
    transfer.http_status = 0;
    transfer.content_range_seen = false;
    transfer.content_range_valid = false;
    transfer.content_range_matches = false;
    transfer.content_range_start = 0;
    transfer.content_range_end = 0;
    transfer.content_range_total = 0;
    transfer.content_range_has_total = false;
    transfer.range_fallback_limit = options.range_fallback_limit;
    transfer.range_fallback_rejected = false;
    transfer.response_region.clear();
    transfer.retry_after = 0;
    transfer.error_body_size = 0;
    transfer.error_body_received = 0;
    transfer.http_buffers->error[0] = '\0';

    const std::string range =
        concat("bytes=", transfer.offset, "-", transfer.offset + transfer.length - 1);
    auto headers = build_headers(transfer.request_headers, range);
    if (!headers)
        return std::unexpected(headers.error());
    transfer.headers = std::move(*headers);

    auto configured = set_options(
        transfer.easy.get(), CURLOPT_URL, transfer.url().c_str(), CURLOPT_WRITEFUNCTION,
        write_callback, CURLOPT_WRITEDATA, &transfer, CURLOPT_HEADERFUNCTION, header_callback,
        CURLOPT_HEADERDATA, &transfer, CURLOPT_PRIVATE, &transfer, CURLOPT_SHARE, share,
        CURLOPT_ERRORBUFFER, transfer.http_buffers->error.data(), CURLOPT_NOSIGNAL, 1L,
        CURLOPT_PATH_AS_IS, 1L, CURLOPT_UNRESTRICTED_AUTH, 0L, CURLOPT_MAXREDIRS, 10L,
        CURLOPT_TCP_KEEPALIVE, 1L, CURLOPT_CONNECTTIMEOUT, options.connect_timeout_seconds,
        CURLOPT_LOW_SPEED_LIMIT, options.low_speed_limit, CURLOPT_LOW_SPEED_TIME,
        options.low_speed_time_seconds, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    if (!configured)
        return configured;
    if (auto protocols = restrict_to_http(transfer.easy.get()); !protocols)
        return protocols;
    if (auto http = apply_http_options(transfer.easy.get(), *transfer.http); !http)
        return http;
    if (transfer.headers) {
        return set_option(transfer.easy.get(), CURLOPT_HTTPHEADER, transfer.headers.get());
    }
    return {};
}

std::expected<std::uint64_t, Failure> size_of(const Locator& locator, CURL* easy, CURLSH* share,
                                              RequestBuilder& request_builder,
                                              const ClientOptions& options) {
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    SizeState state;
    state.easy = easy;
    auto configured =
        set_options(easy, CURLOPT_HEADERFUNCTION, size_header_callback, CURLOPT_HEADERDATA, &state,
                    CURLOPT_WRITEFUNCTION, size_body_callback, CURLOPT_WRITEDATA, &state,
                    CURLOPT_SHARE, share, CURLOPT_ERRORBUFFER, error_buffer.data(),
                    CURLOPT_NOSIGNAL, 1L, CURLOPT_PATH_AS_IS, 1L, CURLOPT_UNRESTRICTED_AUTH, 0L,
                    CURLOPT_MAXREDIRS, 10L, CURLOPT_CONNECTTIMEOUT, options.connect_timeout_seconds,
                    CURLOPT_LOW_SPEED_LIMIT, options.low_speed_limit, CURLOPT_LOW_SPEED_TIME,
                    options.low_speed_time_seconds, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    if (!configured) {
        return std::unexpected(Failure{KARU_ERR_NETWORK, configured.error()});
    }
    if (auto protocols = restrict_to_http(easy); !protocols) {
        return std::unexpected(Failure{KARU_ERR_NETWORK, protocols.error()});
    }

    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937 random{static_cast<std::mt19937::result_type>(seed)};
    std::string region_hint;
    bool region_retried = false;
    bool credentials_retried = false;
    for (int attempt = 0; attempt < options.max_attempts;) {
        auto request = request_builder.prepare(locator, 0, 1, region_hint);
        if (!request) {
            return std::unexpected(Failure{request.error().status, request.error().message});
        }
        auto headers = build_headers(request->headers, "bytes=0-0");
        if (!headers) {
            return std::unexpected(Failure{KARU_ERR_NOMEM, headers.error()});
        }
        if (auto result = set_option(easy, CURLOPT_HTTPHEADER, headers->get()); !result) {
            return std::unexpected(Failure{KARU_ERR_NETWORK, result.error()});
        }
        if (auto result = set_option(easy, CURLOPT_URL, request->url.c_str()); !result) {
            return std::unexpected(Failure{KARU_ERR_NETWORK, result.error()});
        }
        if (auto result = apply_http_options(easy, request->http); !result)
            return std::unexpected(Failure{KARU_ERR_NETWORK, result.error()});
        region_hint = request->routing_region;

        state = SizeState{.easy = easy,
                          .follow_redirects = request->http.follow_redirects,
                          .response_region = {}};
        error_buffer[0] = '\0';
        CURLcode code = curl_easy_perform(easy);
        long http_status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_status);
        if (code == CURLE_WRITE_ERROR && state.cut_short)
            code = CURLE_OK;

        const std::string_view error_body(state.error_body.data(), state.error_body_size);
        if (state.response_region.empty() && locator.resolved.backend == Backend::S3)
            state.response_region = detail::s3_region(error_body);

        if (http_status >= 300 && !region_retried && !state.response_region.empty() &&
            state.response_region != region_hint && locator.resolved.backend == Backend::S3) {
            region_hint = state.response_region;
            region_retried = true;
            continue;
        }

        if (!credentials_retried &&
            detail::credentials_expired(locator.resolved.backend, http_status, error_body)) {
            request_builder.invalidate_credentials(locator);
            credentials_retried = true;
            continue;
        }

        if (code == CURLE_OK && http_status == 416) {
            if (state.unsatisfied_content_range_valid && state.total == 0)
                return std::uint64_t{0};
            if (!state.unsatisfied_content_range_valid) {
                return std::unexpected(
                    Failure{KARU_ERR_HTTP, concat(redact_url(request->url),
                                                  ": invalid Content-Range in HTTP 416")});
            }
        }

        if (http_status == 206 && state.body_exceeded_range) {
            return std::unexpected(
                Failure{KARU_ERR_HTTP,
                        concat(redact_url(request->url), ": size response exceeded bytes 0-0")});
        }

        if (code == CURLE_OK && http_status == 206) {
            if (!state.content_range_valid || !state.have_total || state.body_received != 1) {
                return std::unexpected(
                    Failure{KARU_ERR_HTTP, concat(redact_url(request->url),
                                                  ": partial response has no valid object size")});
            }
            return state.total;
        }

        if (code == CURLE_OK && http_status == 200) {
            if (!state.have_total) {
                curl_off_t length = -1;
                curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
                if (length >= 0) {
                    state.total = static_cast<std::uint64_t>(length);
                    state.have_total = true;
                }
            }
            if (!state.have_total) {
                return std::unexpected(
                    Failure{KARU_ERR_HTTP,
                            concat(redact_url(request->url), ": the server reported no size")});
            }
            return state.total;
        }

        if (code == CURLE_OK && http_status >= 200 && http_status < 300) {
            return std::unexpected(
                Failure{KARU_ERR_HTTP, concat(redact_url(request->url), ": unexpected HTTP ",
                                              http_status, " response to size request")});
        }

        ++attempt;
        const bool request_timeout =
            http_status == 400 && error_body.find("RequestTimeout") != std::string_view::npos;
        if (attempt < options.max_attempts &&
            (detail::transient(code, http_status) || request_timeout)) {
            const int base = 100 << std::min(attempt, 6);
            std::uniform_int_distribution<int> jitter(0, base);
            const int delay = state.retry_after > 0 ? std::min(state.retry_after, 60) * 1000
                                                    : base + jitter(random);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            continue;
        }

        if (code != CURLE_OK && http_status >= 300 && http_status < 400 &&
            request->http.follow_redirects) {
            const char* message =
                error_buffer[0] != '\0' ? error_buffer.data() : curl_easy_strerror(code);
            return std::unexpected(
                Failure{KARU_ERR_NETWORK, concat(redact_url(request->url), ": ", message)});
        }
        if (http_status >= 300) {
            std::string detail = concat(redact_url(request->url), ": HTTP ", http_status);
            if (const std::string body = detail::summarize(error_body); !body.empty()) {
                detail += ": " + body;
            }
            return std::unexpected(
                Failure{detail::status_for_http(http_status), std::move(detail)});
        }
        const char* message =
            error_buffer[0] != '\0' ? error_buffer.data() : curl_easy_strerror(code);
        return std::unexpected(
            Failure{KARU_ERR_NETWORK, concat(redact_url(request->url), ": ", message)});
    }

    return std::unexpected(Failure{KARU_ERR_NETWORK, "size request exhausted its retries"});
}

} // namespace karu::transport
