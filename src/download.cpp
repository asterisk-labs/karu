#include "download.hpp"

#include "client.hpp"
#include "error.hpp"
#include "locator.hpp"
#include "platform.hpp"
#include "runtime/engine.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace karu {
namespace {

constexpr std::uint64_t kProbeSize = UINT64_C(1) << 20;

void add_saturated(std::uint64_t& destination, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - destination)
        destination = std::numeric_limits<std::uint64_t>::max();
    else
        destination += value;
}

karu_status output_error(std::string_view destination, std::string_view action,
                         const std::error_code& error) {
    set_error(concat(destination, ": cannot ", action, ": ", error.message()));
    return KARU_ERR_IO;
}

struct DownloadPart {
    std::uint64_t offset = 0;
    std::size_t length = 0;
};

struct BatchDeleter {
    void operator()(karu_batch* batch) const { karu_batch_free(batch); }
};

struct Slot {
    explicit Slot(std::size_t capacity) : buffer(capacity) {}

    std::vector<std::byte> buffer;
    DownloadPart part;
    std::unique_ptr<karu_batch, BatchDeleter> batch;
};

void add_stats(DownloadSummary& summary, const karu_batch_stats& stats) {
    add_saturated(summary.received_bytes, stats.received_bytes);
    add_saturated(summary.retries, stats.retries);
    add_saturated(summary.throttled, stats.throttled);
    add_saturated(summary.new_connections, stats.new_connections);
    add_saturated(summary.resumed, stats.resumed);
    add_saturated(summary.credential_refreshes, stats.credential_refreshes);
}

void add_stats(DownloadSummary& summary, const transport::RangeStats& stats) {
    add_saturated(summary.received_bytes, stats.received_bytes);
    add_saturated(summary.retries, stats.retries);
    add_saturated(summary.throttled, stats.throttled);
    add_saturated(summary.new_connections, stats.new_connections);
    add_saturated(summary.resumed, stats.resumed);
    add_saturated(summary.credential_refreshes, stats.credential_refreshes);
}

} // namespace

karu_status download(karu_client* client, const karu_locator* locator, std::string_view destination,
                     const DownloadSettings& settings, DownloadSummary& summary) {
    os::OutputFile output;
    if (const std::error_code error = output.create_near(destination, settings.overwrite)) {
        if (error == std::errc::file_exists) {
            set_error(concat(destination, ": destination already exists"));
            return KARU_ERR_IO;
        }
        return output_error(destination, "create temporary output", error);
    }

    const auto probe_capacity = static_cast<std::size_t>(std::min(settings.chunk_size, kProbeSize));
    auto probe = std::make_unique_for_overwrite<std::byte[]>(probe_capacity);
    transport::RangeStats first_stats;
    karu_status status =
        client->acquire_engine()->read_ends(*locator, std::span(probe.get(), probe_capacity), {},
                                            summary.size, summary.etag, &first_stats);
    if (status != KARU_OK)
        return status;
    add_stats(summary, first_stats);

    if (const std::error_code error = output.resize(summary.size))
        return output_error(destination, "resize temporary output", error);

    const auto probe_length = static_cast<std::size_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(probe_capacity), summary.size));
    if (probe_length > 0) {
        if (const std::error_code error = output.write_at(probe.get(), probe_length, 0))
            return output_error(destination, "write temporary output", error);
        summary.downloaded = probe_length;
        if (settings.progress != nullptr &&
            settings.progress(settings.progress_user_data, summary.downloaded, summary.size) == 0) {
            set_error(concat(destination, ": download cancelled by progress callback"));
            return KARU_ERR_CANCELLED;
        }
    }
    probe.reset();

    const std::uint64_t remaining = summary.size - probe_length;
    const std::uint64_t chunks = remaining == 0 ? 0 : 1 + (remaining - 1) / settings.chunk_size;
    const auto slot_count = static_cast<std::size_t>(std::min(settings.parallelism, chunks));
    const auto slot_capacity = static_cast<std::size_t>(std::min(settings.chunk_size, remaining));
    std::vector<std::unique_ptr<Slot>> slots;
    slots.reserve(slot_count);
    for (std::size_t index = 0; index < slot_count; ++index)
        slots.push_back(std::make_unique<Slot>(slot_capacity));

    const auto submit_chunk = [&](Slot& slot, std::uint64_t chunk) {
        const std::uint64_t offset = probe_length + chunk * settings.chunk_size;
        const auto length =
            static_cast<std::size_t>(std::min(settings.chunk_size, summary.size - offset));
        slot.part = DownloadPart{offset, length};
        const karu_req request{
            .locator = locator,
            .offset = offset,
            .length = length,
            .buffer = slot.buffer.data(),
            .tag = &slot.part,
            .if_match = summary.etag.empty() ? nullptr : summary.etag.c_str(),
        };
        karu_submit_options submit = KARU_SUBMIT_OPTIONS_INIT;
        submit.coalesce_gap = 0;
        karu_batch* raw_batch = nullptr;
        const karu_status submitted =
            karu_client_submit_with(client, &request, 1, &submit, &raw_batch);
        if (submitted == KARU_OK)
            slot.batch.reset(raw_batch);
        return submitted;
    };

    std::uint64_t next_chunk = 0;
    for (auto& slot : slots) {
        status = submit_chunk(*slot, next_chunk++);
        if (status != KARU_OK)
            return status;
    }

    std::size_t active = slots.size();
    std::size_t wait_cursor = 0;
    while (active > 0) {
        bool completed_one = false;
        bool waited = false;
        for (std::size_t step = 0; step < slots.size(); ++step) {
            const std::size_t index = (wait_cursor + step) % slots.size();
            Slot& slot = *slots[index];
            if (!slot.batch)
                continue;
            karu_done done{};
            status = karu_batch_next(slot.batch.get(), &done, waited ? 0 : 10);
            waited = true;
            if (status == KARU_TIMEOUT)
                continue;
            if (status != KARU_OK)
                return status;
            if (done.status != KARU_OK)
                return done.status;
            if (done.tag != &slot.part || done.got != slot.part.length) {
                set_error(concat(destination, ": download completed ", done.got, " of ",
                                 slot.part.length, " bytes at offset ", slot.part.offset));
                return KARU_ERR_IO;
            }
            if (const std::error_code error =
                    output.write_at(done.buffer, slot.part.length, slot.part.offset)) {
                return output_error(destination, "write temporary output", error);
            }
            add_saturated(summary.downloaded, done.got);
            if (settings.progress != nullptr &&
                settings.progress(settings.progress_user_data, summary.downloaded, summary.size) ==
                    0) {
                set_error(concat(destination, ": download cancelled by progress callback"));
                return KARU_ERR_CANCELLED;
            }

            karu_batch_stats stats = KARU_BATCH_STATS_INIT;
            status = karu_batch_get_stats(slot.batch.get(), &stats);
            if (status != KARU_OK)
                return status;
            add_stats(summary, stats);
            slot.batch.reset();
            if (next_chunk < chunks) {
                status = submit_chunk(slot, next_chunk++);
                if (status != KARU_OK)
                    return status;
            } else {
                --active;
            }
            wait_cursor = (index + 1) % slots.size();
            completed_one = true;
        }
        if (!completed_one)
            wait_cursor = (wait_cursor + 1) % slots.size();
    }

    if (const std::error_code error = output.publish(settings.overwrite))
        return output_error(destination, "publish downloaded file", error);
    return KARU_OK;
}

} // namespace karu
