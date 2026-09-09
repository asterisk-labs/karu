#include "s3_signing.hpp"

#include "../uri.hpp"
#include "credentials.hpp"
#include "crypto.hpp"
#include "url.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <openssl/sha.h>
#include <ranges>
#include <span>

namespace karu::backends {
namespace {

std::string virtual_host_url(const UrlParts& endpoint, std::string_view bucket,
                             std::string_view key) {
    return endpoint.scheme + "://" + std::string(bucket) + "." + endpoint.authority +
           (endpoint.path == "/" ? "/" : without_trailing_slash(endpoint.path) + "/") +
           encode_path(key);
}

std::string signed_host(const UrlParts& url) {
    const std::string_view default_port = url.scheme == "https" ? ":443" : ":80";
    if (url.authority.ends_with(default_port))
        return url.authority.substr(0, url.authority.size() - default_port.size());
    return url.authority;
}

std::string hex(std::span<const unsigned char> bytes) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = DIGITS[bytes[index] >> 4];
        result[index * 2 + 1] = DIGITS[bytes[index] & 0x0f];
    }
    return result;
}

std::array<unsigned char, SHA256_DIGEST_LENGTH> sha256(std::string_view text) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest.data());
    return digest;
}

} // namespace

bool valid_aws_region(std::string_view region) noexcept {
    return !region.empty() && std::ranges::all_of(region, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-';
    });
}

std::expected<PreparedRequest, RequestError>
prepare_s3_get(const S3GetRequest& target, const ProviderCredentials* credentials,
               std::uint64_t first, std::uint64_t length, std::string_view if_match) {
    auto endpoint = split_url(target.endpoint);
    if (!endpoint)
        return std::unexpected(endpoint.error());
    if (!endpoint->query.empty())
        return std::unexpected(RequestError{KARU_ERR_CONFIG, std::string(target.endpoint_option) +
                                                                 " cannot contain a query"});

    std::string url = target.virtual_hosting
                          ? virtual_host_url(*endpoint, target.bucket, target.key)
                          : append_object(target.endpoint, target.bucket, target.key);
    if (credentials == nullptr) {
        PreparedRequest request{std::move(url), {}};
        if (!if_match.empty())
            request.headers.emplace_back("If-Match", if_match);
        return request;
    }

    auto url_parts = split_url(url);
    if (!url_parts)
        return std::unexpected(url_parts.error());
    const std::time_t now = std::time(nullptr);
    const std::string timestamp = date_utc(now, "%Y%m%dT%H%M%SZ");
    const std::string date = timestamp.substr(0, 8);
    const std::string range = range_header(first, length);
    const std::string payload = "UNSIGNED-PAYLOAD";

    std::vector<Header> signed_headers{{"host", signed_host(*url_parts)},
                                       {"range", range},
                                       {"x-amz-content-sha256", payload},
                                       {"x-amz-date", timestamp}};
    if (!if_match.empty())
        signed_headers.emplace_back("if-match", if_match);
    if (!credentials->session_token.empty())
        signed_headers.emplace_back("x-amz-security-token", credentials->session_token);
    if (lower(std::string(target.request_payer)) == "requester")
        signed_headers.emplace_back("x-amz-request-payer", "requester");
    std::ranges::sort(signed_headers, {}, &Header::first);

    std::string canonical_headers;
    std::string signed_names;
    for (const auto& [name, value] : signed_headers) {
        canonical_headers += lower(name) + ":" + trim(value) + "\n";
        if (!signed_names.empty())
            signed_names.push_back(';');
        signed_names += lower(name);
    }
    const std::string canonical_request = "GET\n" + url_parts->path + "\n" + url_parts->query +
                                          "\n" + canonical_headers + "\n" + signed_names + "\n" +
                                          payload;
    const std::string scope = date + "/" + std::string(target.region) + "/s3/aws4_request";
    const std::string string_to_sign =
        "AWS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" + hex(sha256(canonical_request));

    const std::string initial = "AWS4" + credentials->secret_access_key;
    auto date_key = hmac_sha256(
        std::span(reinterpret_cast<const unsigned char*>(initial.data()), initial.size()), date);
    auto region_key = hmac_sha256(date_key, target.region);
    auto service_key = hmac_sha256(region_key, "s3");
    auto signing_key = hmac_sha256(service_key, "aws4_request");
    const std::string signature = hex(hmac_sha256(signing_key, string_to_sign));

    std::vector<Header> headers;
    headers.reserve(signed_headers.size() + 1);
    for (const auto& [name, value] : signed_headers) {
        if (name != "host" && name != "range")
            headers.emplace_back(name, value);
    }
    headers.emplace_back("Authorization",
                         "AWS4-HMAC-SHA256 Credential=" + credentials->access_key_id + "/" + scope +
                             ", SignedHeaders=" + signed_names + ", Signature=" + signature);
    PreparedRequest request{std::move(url), std::move(headers)};
    request.http.follow_redirects = false;
    return request;
}

} // namespace karu::backends
