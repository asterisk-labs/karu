#ifndef KARU_DOWNLOAD_HPP
#define KARU_DOWNLOAD_HPP

#include "karu/karu.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace karu {

struct DownloadSettings {
    std::uint64_t chunk_size = KARU_DOWNLOAD_CHUNK_SIZE_DEFAULT;
    std::uint64_t parallelism = KARU_DOWNLOAD_PARALLELISM_DEFAULT;
    bool overwrite = false;
    karu_download_progress progress = nullptr;
    void* progress_user_data = nullptr;
};

struct DownloadSummary {
    std::uint64_t size = 0;
    std::uint64_t downloaded = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t retries = 0;
    std::uint64_t throttled = 0;
    std::uint64_t new_connections = 0;
    std::uint64_t resumed = 0;
    std::uint64_t credential_refreshes = 0;
    std::string etag;
};

karu_status download(karu_client* client, const karu_locator* locator, std::string_view destination,
                     const DownloadSettings& settings, DownloadSummary& summary);

} // namespace karu

#endif
