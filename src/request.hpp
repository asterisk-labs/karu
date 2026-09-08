#ifndef KARU_REQUEST_HPP
#define KARU_REQUEST_HPP

#include "karu/karu.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace karu {

struct PreparedRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct RequestError {
    karu_status status = KARU_ERR_CREDENTIALS;
    std::string message;
};

struct ProviderCredentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    std::string bearer_token;
    std::string sas_token;
    std::string account_name;
    std::string region;
    std::string cache_prefix;
    std::int64_t expires_at = 0;
};

} // namespace karu

#endif
